package com.kappara.service.algorithm;

import com.kappara.model.AlgorithmStatus;
import com.kappara.model.Position;
import com.kappara.model.Quote;
import com.kappara.service.MarketDataService;
import com.kappara.service.RiskEngine;
import com.kappara.service.TradingEngine;
import lombok.RequiredArgsConstructor;
import lombok.extern.slf4j.Slf4j;
import org.springframework.stereotype.Component;

import java.time.Instant;
import java.util.*;

/**
 * Delta-neutral hedging strategy.
 *
 * Maintains long positions in AAPL and MSFT (stock alpha book).
 * Hedges the aggregate beta-weighted delta exposure with SPY.
 *
 * Rebalance logic:
 *  1. Compute net portfolio delta = sum(qty * beta * price) for AAPL + MSFT.
 *  2. If |net delta| > DELTA_THRESHOLD, buy/sell SPY to bring net delta ≈ 0.
 *  3. Only execute when RiskEngine reports risk within limits.
 *
 * Initial positions are seeded once at startup.
 */
@Slf4j
@Component
@RequiredArgsConstructor
public class DeltaHedgingAlgorithm implements TradingAlgorithm {

    public static final String NAME = "Delta Hedging (AAPL/MSFT → SPY)";

    private static final double STOCK_NOTIONAL  = 20_000.0; // per stock leg
    private static final double DELTA_THRESHOLD = 5_000.0;  // rebalance when |delta| > $5K

    private final MarketDataService marketDataService;
    private final TradingEngine     tradingEngine;
    private final RiskEngine        riskEngine;

    private volatile AlgorithmStatus status;
    private boolean initialised = false;

    @Override
    public String getName() { return NAME; }

    @Override
    public void onTick() {
        seedInitialPositions();

        Quote aapl = marketDataService.getQuote("AAPL");
        Quote msft = marketDataService.getQuote("MSFT");
        Quote spy  = marketDataService.getQuote("SPY");
        if (aapl == null || msft == null || spy == null) return;

        // Beta-weighted delta for stock legs only
        double aaplBeta = marketDataService.getInstruments().get("AAPL").getBeta();
        double msftBeta = marketDataService.getInstruments().get("MSFT").getBeta();
        double spyBeta  = marketDataService.getInstruments().get("SPY").getBeta();

        int aaplQty = tradingEngine.getPositionQuantity("AAPL");
        int msftQty = tradingEngine.getPositionQuantity("MSFT");
        int spyQty  = tradingEngine.getPositionQuantity("SPY");

        double stockDelta = aaplQty * aaplBeta * aapl.getPrice()
                          + msftQty * msftBeta * msft.getPrice();
        double hedgeDelta = spyQty * spyBeta * spy.getPrice();
        double netDelta   = stockDelta + hedgeDelta;

        if (Math.abs(netDelta) > DELTA_THRESHOLD) {
            if (!riskEngine.isRiskWithinLimits()) {
                buildStatus("BLOCKED_BY_RISK",
                        String.format("Net delta $%.0f exceeds threshold but risk limits breached", netDelta),
                        netDelta, stockDelta, hedgeDelta);
                return;
            }
            rebalanceHedge(netDelta, spy.getPrice(), stockDelta, hedgeDelta);
        } else {
            buildStatus("HOLD",
                    String.format("Net delta $%.0f within threshold $%.0f — no rebalance", netDelta, DELTA_THRESHOLD),
                    netDelta, stockDelta, hedgeDelta);
        }
    }

    /** Buy AAPL + MSFT once to seed the alpha book. */
    private void seedInitialPositions() {
        if (initialised) return;
        initialised = true;
        Quote aapl = marketDataService.getQuote("AAPL");
        Quote msft = marketDataService.getQuote("MSFT");
        if (aapl == null || msft == null) { initialised = false; return; }

        int aaplQty = Math.max(1, (int) (STOCK_NOTIONAL / aapl.getPrice()));
        int msftQty = Math.max(1, (int) (STOCK_NOTIONAL / msft.getPrice()));
        tradingEngine.execute("AAPL", aaplQty, "BUY", NAME, "Seed alpha book");
        tradingEngine.execute("MSFT", msftQty, "BUY", NAME, "Seed alpha book");
        log.info("Delta hedger seeded: {} AAPL, {} MSFT", aaplQty, msftQty);
    }

    /**
     * Sell or buy SPY to offset current net delta.
     * Required SPY hedge = -stockDelta / (spyBeta * spyPrice)
     */
    private void rebalanceHedge(double netDelta, double spyPrice,
                                 double stockDelta, double hedgeDelta) {
        double spyBeta      = marketDataService.getInstruments().get("SPY").getBeta();
        int currentSpyQty   = tradingEngine.getPositionQuantity("SPY");
        int targetSpyQty    = -(int) (stockDelta / (spyBeta * spyPrice));
        int delta           = targetSpyQty - currentSpyQty;

        if (delta == 0) return;

        String side   = delta > 0 ? "BUY" : "SELL";
        String reason = String.format("Rebalance hedge: net delta $%.0f → target SPY qty %d (delta %d)", netDelta, targetSpyQty, delta);
        tradingEngine.execute("SPY", Math.abs(delta), side, NAME, reason);

        buildStatus("HEDGE", reason, netDelta, stockDelta, hedgeDelta);
    }

    private void buildStatus(String decision, String reasoning,
                              double netDelta, double stockDelta, double hedgeDelta) {
        Map<String, Double> m = new LinkedHashMap<>();
        m.put("netDelta",     Math.round(netDelta    * 100.0) / 100.0);
        m.put("stockDelta",   Math.round(stockDelta  * 100.0) / 100.0);
        m.put("hedgeDelta",   Math.round(hedgeDelta  * 100.0) / 100.0);
        m.put("threshold",    DELTA_THRESHOLD);
        m.put("aaplQty",      (double) tradingEngine.getPositionQuantity("AAPL"));
        m.put("msftQty",      (double) tradingEngine.getPositionQuantity("MSFT"));
        m.put("spyHedgeQty",  (double) tradingEngine.getPositionQuantity("SPY"));

        double signal = Math.max(-1, Math.min(1, netDelta / (DELTA_THRESHOLD * 2)));
        status = AlgorithmStatus.builder()
                .name(NAME).active(true)
                .lastDecision(decision).reasoning(reasoning)
                .signal(signal).metrics(m).lastUpdate(Instant.now())
                .build();
    }

    @Override
    public AlgorithmStatus getStatus() { return status; }
}
