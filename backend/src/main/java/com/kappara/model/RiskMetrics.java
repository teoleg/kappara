package com.kappara.model;

import lombok.Builder;
import lombok.Data;
import java.time.Instant;

@Data
@Builder
public class RiskMetrics {
    private double portfolioNAV;
    private double totalUnrealizedPnL;
    private double totalRealizedPnL;
    private double valueAtRisk;        // 95% 1-day VaR in USD
    private double varPercent;         // VaR as % of NAV
    private double netDelta;           // net portfolio delta in USD
    private double grossExposure;      // sum |market value|
    private double netExposure;        // sum market values
    private boolean riskWithinLimits;
    private String riskStatus;         // GREEN, YELLOW, RED
    private Instant timestamp;
}
