package com.kappara.service.algorithm;

import com.kappara.model.AlgorithmStatus;
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
 * Statistical arbitrage on the SPY / QQQ spread.
 *
 * Entry signals (z-score of rolling ratio):
 *   z > +2  → ratio HIGH: short SPY, long QQQ  (expect reversion down)
 *   z < -2  → ratio LOW:  long SPY, short QQQ   (expect reversion up)
 * Exit signal:
 *   |z| < 0.5 → spread reverted, close both legs
 *
 * Gate: trades are only executed when RiskEngine reports GREEN or YELLOW.
 */
@Slf4j
@Component
@RequiredArgsConstructor
public class PairsTradingAlgorithm implements TradingAlgorithm {

    public static final String NAME = "Pairs Trading (SPY/QQQ)";

    private static final int    WINDOW          = 20;
    private static final double ENTRY_THRESHOLD = 2.0;
    private static final double EXIT_THRESHOLD  = 0.5;
    private static final double LEG_NOTIONAL    = 10_000.0;

    private final MarketDataService marketDataService;
    private final TradingEngine     tradingEngine;
    private final RiskEngine        riskEngine;

    private final Deque<Double> ratioHistory = new ArrayDeque<>();
    private volatile AlgorithmStatus status;
    private boolean inPosition = false;

    @Override
    public String getName() { return NAME; }

    @Override
    public void onTick() {
        Quote spy = marketDataService.getQuote("SPY");
        Quote qqq = marketDataService.getQuote("QQQ");
        if (spy == null || qqq == null) return;

        double ratio = spy.getPrice() / qqq.getPrice();
        ratioHistory.addLast(ratio);
        if (ratioHistory.size() > WINDOW) ratioHistory.pollFirst();

        if (ratioHistory.size() < WINDOW) {
            buildStatus("WARMUP", 0, "Warming up (" + ratioHistory.size() + "/" + WINDOW + ")", ratio, 0, 0);
            return;
        }

        double mean   = ratioHistory.stream().mapToDouble(Double::doubleValue).average().orElse(ratio);
        double std    = stddev(ratioHistory, mean);
        double zScore = std > 0 ? (ratio - mean) / std : 0.0;

        if (inPosition && Math.abs(zScore) < EXIT_THRESHOLD) {
            closeLegs(zScore, "Spread reverted");
        } else if (!inPosition && Math.abs(zScore) > ENTRY_THRESHOLD) {
            if (!riskEngine.isRiskWithinLimits()) {
                buildStatus("BLOCKED_BY_RISK", zScore,
                        String.format("z=%.2f — signal present but risk limits breached, no trade", zScore),
                        ratio, mean, std);
            } else {
                openLegs(zScore, spy.getPrice(), qqq.getPrice(), ratio, mean, std);
            }
        } else {
            buildStatus(inPosition ? "HOLD (in position)" : "HOLD (no signal)", zScore,
                    String.format("z=%.2f | ratio=%.4f | mean=%.4f | std=%.4f", zScore, ratio, mean, std),
                    ratio, mean, std);
        }
    }

    private void openLegs(double zScore, double spyPrice, double qqqPrice,
                          double ratio, double mean, double std) {
        int spyQty = Math.max(1, (int) (LEG_NOTIONAL / spyPrice));
        int qqqQty = Math.max(1, (int) (LEG_NOTIONAL / qqqPrice));
        String reason = String.format("Pairs entry z=%.2f", zScore);

        if (zScore > 0) {
            tradingEngine.execute("SPY", spyQty, "SELL", NAME, reason + " | SHORT SPY");
            tradingEngine.execute("QQQ", qqqQty, "BUY",  NAME, reason + " | LONG QQQ");
            buildStatus("TRADE", zScore, "SHORT SPY / LONG QQQ — " + reason, ratio, mean, std);
        } else {
            tradingEngine.execute("SPY", spyQty, "BUY",  NAME, reason + " | LONG SPY");
            tradingEngine.execute("QQQ", qqqQty, "SELL", NAME, reason + " | SHORT QQQ");
            buildStatus("TRADE", zScore, "LONG SPY / SHORT QQQ — " + reason, ratio, mean, std);
        }
        inPosition = true;
    }

    private void closeLegs(double zScore, String reason) {
        int spyQty = tradingEngine.getPositionQuantity("SPY");
        int qqqQty = tradingEngine.getPositionQuantity("QQQ");
        String msg  = String.format("%s | z=%.2f", reason, zScore);

        if (spyQty != 0) tradingEngine.execute("SPY", Math.abs(spyQty), spyQty > 0 ? "SELL" : "BUY", NAME, msg);
        if (qqqQty != 0) tradingEngine.execute("QQQ", Math.abs(qqqQty), qqqQty > 0 ? "SELL" : "BUY", NAME, msg);

        inPosition = false;
        buildStatus("CLOSE", zScore, "Legs closed — " + msg, 0, 0, 0);
    }

    private void buildStatus(String decision, double zScore, String reasoning,
                              double ratio, double mean, double std) {
        Map<String, Double> m = new LinkedHashMap<>();
        m.put("zScore", round(zScore));
        m.put("ratio",  round(ratio));
        m.put("mean",   round(mean));
        m.put("stddev", round(std));
        m.put("window", (double) WINDOW);
        m.put("inPosition", inPosition ? 1.0 : 0.0);

        status = AlgorithmStatus.builder()
                .name(NAME).active(true)
                .lastDecision(decision).reasoning(reasoning)
                .signal(Math.max(-1, Math.min(1, zScore / ENTRY_THRESHOLD)))
                .metrics(m).lastUpdate(Instant.now())
                .build();
    }

    private double stddev(Deque<Double> vals, double mean) {
        return Math.sqrt(vals.stream().mapToDouble(v -> (v - mean) * (v - mean)).average().orElse(0));
    }

    private double round(double v) { return Math.round(v * 10000.0) / 10000.0; }

    @Override
    public AlgorithmStatus getStatus() { return status; }
}
