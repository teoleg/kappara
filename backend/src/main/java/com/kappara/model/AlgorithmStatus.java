package com.kappara.model;

import lombok.Builder;
import lombok.Data;
import java.time.Instant;
import java.util.Map;

@Data
@Builder
public class AlgorithmStatus {
    private String name;
    private boolean active;
    private String lastDecision;   // TRADE, HOLD, BLOCKED_BY_RISK, CLOSE
    private String reasoning;
    private double signal;         // signal strength: -1 to +1
    private Map<String, Double> metrics;  // algo-specific metrics
    private Instant lastUpdate;
}
