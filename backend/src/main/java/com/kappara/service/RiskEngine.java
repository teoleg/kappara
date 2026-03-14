package com.kappara.service;

import com.kappara.model.Position;
import com.kappara.model.RiskMetrics;
import lombok.RequiredArgsConstructor;
import lombok.extern.slf4j.Slf4j;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.messaging.simp.SimpMessagingTemplate;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Service;

import java.time.Instant;
import java.util.*;

/**
 * Calculates real-time risk metrics every second:
 *  - Portfolio VaR (95%, 1-day) via historical simulation
 *  - Net delta exposure (beta-weighted)
 *  - Gross / net market exposure
 *  - Unrealized + realized PnL
 *
 * Risk limits (application.yml):
 *  - VaR < maxVarPercent % of NAV  → GREEN
 *  - |net delta| < maxNetDelta USD  → GREEN
 *  Both breached by >50% → RED, otherwise YELLOW
 */
@Slf4j
@Service
@RequiredArgsConstructor
public class RiskEngine {

    private final MarketDataService marketDataService;
    private final TradingEngine tradingEngine;
    private final SimpMessagingTemplate messagingTemplate;

    @Value("${trading.risk.max-var-percent:2.0}")
    private double maxVarPercent;

    @Value("${trading.risk.max-net-delta:50000}")
    private double maxNetDelta;

    private volatile RiskMetrics latestMetrics;

    @Scheduled(fixedRate = 1000)
    public void compute() {
        tradingEngine.refreshPositionPrices();

        List<Position> positions = tradingEngine.getOpenPositions();
        double cash = tradingEngine.getCash();

        double unrealizedPnL = positions.stream().mapToDouble(Position::getUnrealizedPnL).sum();
        double realizedPnL   = positions.stream().mapToDouble(Position::getRealizedPnL).sum();
        double grossExposure = positions.stream().mapToDouble(p -> Math.abs(p.getMarketValue())).sum();
        double netExposure   = positions.stream().mapToDouble(Position::getMarketValue).sum();
        double netDelta      = positions.stream().mapToDouble(Position::getDeltaExposure).sum();
        double nav           = cash + netExposure;

        double var    = computeVaR(positions, nav);
        double varPct = nav > 0 ? (var / nav) * 100.0 : 0.0;

        boolean varOk   = varPct < maxVarPercent;
        boolean deltaOk = Math.abs(netDelta) < maxNetDelta;
        boolean withinLimits = varOk && deltaOk;

        String status;
        if (withinLimits) {
            status = "GREEN";
        } else if (varPct < maxVarPercent * 1.5 && Math.abs(netDelta) < maxNetDelta * 1.5) {
            status = "YELLOW";
        } else {
            status = "RED";
        }

        latestMetrics = RiskMetrics.builder()
                .portfolioNAV(nav)
                .totalUnrealizedPnL(unrealizedPnL)
                .totalRealizedPnL(realizedPnL)
                .valueAtRisk(var)
                .varPercent(varPct)
                .netDelta(netDelta)
                .grossExposure(grossExposure)
                .netExposure(netExposure)
                .riskWithinLimits(withinLimits)
                .riskStatus(status)
                .timestamp(Instant.now())
                .build();

        messagingTemplate.convertAndSend("/topic/risk", latestMetrics);
    }

    /**
     * Historical simulation VaR:
     * Simulates portfolio P&L for each historical tick and takes 5th percentile loss.
     */
    private double computeVaR(List<Position> positions, double nav) {
        if (positions.isEmpty() || nav <= 0) return 0.0;

        Map<String, Deque<Double>> history = marketDataService.getPriceHistory();

        int windowSize = positions.stream()
                .map(p -> history.getOrDefault(p.getSymbol(), new ArrayDeque<>()))
                .mapToInt(Deque::size)
                .min().orElse(0);

        if (windowSize < 2) return 0.0;

        Map<String, double[]> arrays = new HashMap<>();
        for (Position pos : positions) {
            Deque<Double> dq = history.get(pos.getSymbol());
            if (dq != null) arrays.put(pos.getSymbol(), dq.stream().mapToDouble(Double::doubleValue).toArray());
        }

        List<Double> portfolioReturns = new ArrayList<>();
        for (int i = 1; i < windowSize; i++) {
            double pReturn = 0.0;
            for (Position pos : positions) {
                double[] prices = arrays.get(pos.getSymbol());
                if (prices == null || i >= prices.length) continue;
                double weight = pos.getMarketValue() / nav;
                double ret    = (prices[i] - prices[i - 1]) / prices[i - 1];
                pReturn += weight * ret;
            }
            portfolioReturns.add(pReturn);
        }

        if (portfolioReturns.isEmpty()) return 0.0;
        Collections.sort(portfolioReturns);
        double worstReturn = portfolioReturns.get(Math.max((int) Math.floor(0.05 * portfolioReturns.size()), 0));
        return Math.max(0.0, -worstReturn * Math.abs(nav));
    }

    public RiskMetrics getLatestMetrics() { return latestMetrics; }

    public boolean isRiskWithinLimits() {
        return latestMetrics == null || latestMetrics.isRiskWithinLimits();
    }
}
