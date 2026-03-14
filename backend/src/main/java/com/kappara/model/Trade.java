package com.kappara.model;

import lombok.Builder;
import lombok.Data;
import java.time.Instant;

@Data
@Builder
public class Trade {
    private String tradeId;
    private String symbol;
    private int quantity;
    private double price;
    private String side;      // BUY or SELL
    private String strategy;
    private String reason;
    private Instant timestamp;
}
