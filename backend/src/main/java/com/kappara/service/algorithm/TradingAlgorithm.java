package com.kappara.service.algorithm;

import com.kappara.model.AlgorithmStatus;

/**
 * Interface for all trading algorithms.
 * Implementations must check RiskEngine.isRiskWithinLimits() before executing trades.
 */
public interface TradingAlgorithm {
    String getName();
    void onTick();
    AlgorithmStatus getStatus();
}
