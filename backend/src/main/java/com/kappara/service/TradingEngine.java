package com.kappara.service;

import com.kappara.model.Instrument;
import com.kappara.model.Position;
import com.kappara.model.Quote;
import com.kappara.model.Trade;
import lombok.Getter;
import lombok.RequiredArgsConstructor;
import lombok.extern.slf4j.Slf4j;
import org.springframework.messaging.simp.SimpMessagingTemplate;
import org.springframework.stereotype.Service;

import java.time.Instant;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

@Slf4j
@Service
@RequiredArgsConstructor
public class TradingEngine {

    private final MarketDataService marketDataService;
    private final SimpMessagingTemplate messagingTemplate;

    // Symbol -> Position
    @Getter
    private final Map<String, Position> positions = new ConcurrentHashMap<>();

    // Ordered trade log (most recent first)
    @Getter
    private final Deque<Trade> tradeLog = new ArrayDeque<>();

    private static final int MAX_TRADE_LOG = 50;

    // Starting cash
    @Getter
    private double cash = 100_000.0;

    private final AtomicLong tradeCounter = new AtomicLong(0);

    /**
     * Execute a market order. Fills instantly at current bid/ask.
     * Returns the executed trade or null if rejected.
     */
    public Trade execute(String symbol, int quantity, String side, String strategy, String reason) {
        Quote quote = marketDataService.getQuote(symbol);
        if (quote == null) {
            log.warn("No quote for symbol {}", symbol);
            return null;
        }

        double fillPrice = "BUY".equals(side) ? quote.getAsk() : quote.getBid();
        double cost = fillPrice * Math.abs(quantity);

        // Check cash for buys
        if ("BUY".equals(side) && cost > cash + grossShortCredit()) {
            log.warn("Insufficient cash for {} {} {}: cost={} cash={}", side, quantity, symbol, cost, cash);
            return null;
        }

        // Update cash
        if ("BUY".equals(side)) {
            cash -= cost;
        } else {
            cash += cost;
        }

        // Update position
        updatePosition(symbol, quantity, fillPrice, side, strategy);

        // Create trade record
        Trade trade = Trade.builder()
                .tradeId("T" + tradeCounter.incrementAndGet())
                .symbol(symbol)
                .quantity(Math.abs(quantity))
                .price(fillPrice)
                .side(side)
                .strategy(strategy)
                .reason(reason)
                .timestamp(Instant.now())
                .build();

        synchronized (tradeLog) {
            tradeLog.addFirst(trade);
            if (tradeLog.size() > MAX_TRADE_LOG) tradeLog.removeLast();
        }

        log.info("TRADE: {} {} {} @ {:.2f} | {}", side, quantity, symbol, fillPrice, reason);
        broadcastTrade(trade);
        broadcastPositions();
        return trade;
    }

    private void updatePosition(String symbol, int quantity, double fillPrice, String side, String strategy) {
        int signed = "BUY".equals(side) ? Math.abs(quantity) : -Math.abs(quantity);
        Instrument inst = marketDataService.getInstruments().get(symbol);
        double beta = inst != null ? inst.getBeta() : 1.0;

        positions.compute(symbol, (sym, existing) -> {
            if (existing == null || existing.getQuantity() == 0) {
                return Position.builder()
                        .symbol(symbol)
                        .quantity(signed)
                        .avgCost(fillPrice)
                        .currentPrice(fillPrice)
                        .marketValue(signed * fillPrice)
                        .unrealizedPnL(0.0)
                        .realizedPnL(0.0)
                        .deltaExposure(signed * beta * fillPrice)
                        .strategy(strategy)
                        .build();
            }

            int newQty = existing.getQuantity() + signed;
            double realizedPnL = existing.getRealizedPnL();

            // Closing or reducing: realize PnL
            if (Integer.signum(existing.getQuantity()) != Integer.signum(signed)
                    && existing.getQuantity() != 0) {
                int closingQty = Math.min(Math.abs(signed), Math.abs(existing.getQuantity()));
                realizedPnL += closingQty * (fillPrice - existing.getAvgCost())
                        * Math.signum(existing.getQuantity());
            }

            double newAvgCost = newQty == 0 ? existing.getAvgCost()
                    : (existing.getAvgCost() * existing.getQuantity() + fillPrice * signed)
                    / newQty;

            return Position.builder()
                    .symbol(symbol)
                    .quantity(newQty)
                    .avgCost(Math.abs(newAvgCost))
                    .currentPrice(fillPrice)
                    .marketValue(newQty * fillPrice)
                    .unrealizedPnL(newQty * (fillPrice - Math.abs(newAvgCost)))
                    .realizedPnL(realizedPnL)
                    .deltaExposure(newQty * beta * fillPrice)
                    .strategy(strategy)
                    .build();
        });

        // Remove flat positions
        positions.entrySet().removeIf(e -> e.getValue().getQuantity() == 0);
    }

    /** Refresh mark-to-market prices for all positions. */
    public void refreshPositionPrices() {
        positions.replaceAll((symbol, pos) -> {
            Quote q = marketDataService.getQuote(symbol);
            if (q == null) return pos;
            Instrument inst = marketDataService.getInstruments().get(symbol);
            double beta = inst != null ? inst.getBeta() : 1.0;
            double price = q.getPrice();
            double mv = pos.getQuantity() * price;
            double uPnL = pos.getQuantity() * (price - pos.getAvgCost());
            return Position.builder()
                    .symbol(symbol)
                    .quantity(pos.getQuantity())
                    .avgCost(pos.getAvgCost())
                    .currentPrice(price)
                    .marketValue(mv)
                    .unrealizedPnL(uPnL)
                    .realizedPnL(pos.getRealizedPnL())
                    .deltaExposure(pos.getQuantity() * beta * price)
                    .strategy(pos.getStrategy())
                    .build();
        });
        broadcastPositions();
    }

    public List<Position> getOpenPositions() {
        return new ArrayList<>(positions.values());
    }

    public int getPositionQuantity(String symbol) {
        Position p = positions.get(symbol);
        return p != null ? p.getQuantity() : 0;
    }

    private double grossShortCredit() {
        return positions.values().stream()
                .filter(p -> p.getQuantity() < 0)
                .mapToDouble(p -> Math.abs(p.getMarketValue()))
                .sum();
    }

    private void broadcastTrade(Trade trade) {
        messagingTemplate.convertAndSend("/topic/trades", trade);
    }

    private void broadcastPositions() {
        messagingTemplate.convertAndSend("/topic/positions", getOpenPositions());
    }
}
