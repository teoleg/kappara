package com.kappara.model;

import lombok.Data;

@Data
public class Instrument {
    private String symbol;
    private String name;
    private String type; // STOCK or ETF
    private double seedPrice;
    private double volatility; // annualized
    private double beta;       // vs SPY
}
