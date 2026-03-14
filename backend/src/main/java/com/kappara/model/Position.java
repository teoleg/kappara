package com.kappara.model;

import lombok.Builder;
import lombok.Data;

@Data
@Builder
public class Position {
    private String symbol;
    private int quantity;          // positive = long, negative = short
    private double avgCost;
    private double currentPrice;
    private double marketValue;
    private double unrealizedPnL;
    private double realizedPnL;
    private double deltaExposure;  // quantity * beta * price
    private String strategy;
}
