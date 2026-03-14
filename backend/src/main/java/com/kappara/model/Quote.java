package com.kappara.model;

import lombok.Builder;
import lombok.Data;
import java.time.Instant;

@Data
@Builder
public class Quote {
    private String symbol;
    private double price;
    private double bid;
    private double ask;
    private double volume;
    private double change;
    private double changePercent;
    private Instant timestamp;
}
