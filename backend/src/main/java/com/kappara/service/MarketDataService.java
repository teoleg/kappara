package com.kappara.service;

import com.kappara.model.Instrument;
import com.kappara.model.Quote;
import lombok.Getter;
import lombok.RequiredArgsConstructor;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.messaging.simp.SimpMessagingTemplate;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Service;

import jakarta.annotation.PostConstruct;
import java.time.Instant;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Simulates real-time market data using Geometric Brownian Motion.
 * SPY and QQQ are correlated (rho=0.95). Stocks correlated with SPY via beta.
 * Replace this service with a real WebSocket market feed (e.g. Alpaca, Finnhub)
 * by injecting quotes into currentQuotes and calling broadcastQuotes().
 */
@Service
@RequiredArgsConstructor
public class MarketDataService {

    private final SimpMessagingTemplate messagingTemplate;

    // Symbol -> instrument config from application.yml
    @Getter
    private final Map<String, Instrument> instruments = new ConcurrentHashMap<>();

    // Symbol -> current quote
    @Getter
    private final Map<String, Quote> currentQuotes = new ConcurrentHashMap<>();

    // Symbol -> rolling price history (last 100 ticks)
    @Getter
    private final Map<String, Deque<Double>> priceHistory = new ConcurrentHashMap<>();

    private final Random random = new Random(42);

    // Market-wide factor — common shock shared across all instruments
    private double marketFactor = 0.0;

    // Annualized drift: small positive (bull market simulation)
    private static final double DRIFT = 0.08;
    // Seconds per tick
    private static final double DT = 1.0 / (252.0 * 6.5 * 3600);
    // Correlation of stocks/ETFs to market factor
    private static final double MARKET_CORR = 0.70;
    // History window
    private static final int HISTORY_SIZE = 100;

    @Value("${trading.instruments:#{null}}")
    private List<Map<String, Object>> instrumentsConfig;

    @PostConstruct
    public void init() {
        // Seed instruments from config or use defaults
        seedInstruments();
        instruments.values().forEach(inst -> {
            priceHistory.put(inst.getSymbol(), new ArrayDeque<>(HISTORY_SIZE));
            Quote initial = buildQuote(inst.getSymbol(), inst.getSeedPrice(), 0.0);
            currentQuotes.put(inst.getSymbol(), initial);
            priceHistory.get(inst.getSymbol()).addLast(inst.getSeedPrice());
        });
    }

    private void seedInstruments() {
        // AAPL
        Instrument aapl = new Instrument();
        aapl.setSymbol("AAPL"); aapl.setName("Apple Inc."); aapl.setType("STOCK");
        aapl.setSeedPrice(180.0); aapl.setVolatility(0.25); aapl.setBeta(1.2);
        // MSFT
        Instrument msft = new Instrument();
        msft.setSymbol("MSFT"); msft.setName("Microsoft Corp."); msft.setType("STOCK");
        msft.setSeedPrice(380.0); msft.setVolatility(0.22); msft.setBeta(1.1);
        // TSLA
        Instrument tsla = new Instrument();
        tsla.setSymbol("TSLA"); tsla.setName("Tesla Inc."); tsla.setType("STOCK");
        tsla.setSeedPrice(250.0); tsla.setVolatility(0.45); tsla.setBeta(1.8);
        // SPY
        Instrument spy = new Instrument();
        spy.setSymbol("SPY"); spy.setName("SPDR S&P 500 ETF"); spy.setType("ETF");
        spy.setSeedPrice(480.0); spy.setVolatility(0.15); spy.setBeta(1.0);
        // QQQ
        Instrument qqq = new Instrument();
        qqq.setSymbol("QQQ"); qqq.setName("Invesco QQQ Trust"); qqq.setType("ETF");
        qqq.setSeedPrice(415.0); qqq.setVolatility(0.18); qqq.setBeta(1.15);

        instruments.put("AAPL", aapl);
        instruments.put("MSFT", msft);
        instruments.put("TSLA", tsla);
        instruments.put("SPY", spy);
        instruments.put("QQQ", qqq);
    }

    @Scheduled(fixedRateString = "${trading.simulation.tick-interval-ms:1000}")
    public void tick() {
        // Generate market-wide shock for this tick
        marketFactor = random.nextGaussian();

        instruments.values().forEach(inst -> {
            Quote prev = currentQuotes.get(inst.getSymbol());
            double newPrice = nextPrice(inst, prev.getPrice());
            double change = newPrice - prev.getPrice();
            double changePercent = (change / prev.getPrice()) * 100.0;

            Quote quote = buildQuote(inst.getSymbol(), newPrice, change);
            currentQuotes.put(inst.getSymbol(), quote);

            Deque<Double> hist = priceHistory.get(inst.getSymbol());
            if (hist.size() >= HISTORY_SIZE) hist.pollFirst();
            hist.addLast(newPrice);
        });

        broadcastQuotes();
    }

    private double nextPrice(Instrument inst, double currentPrice) {
        double sigma = inst.getVolatility();
        double beta = inst.getBeta();

        // Correlated GBM: idiosyncratic + market factor
        double idio = random.nextGaussian();
        double returnNoise = MARKET_CORR * beta * marketFactor
                + Math.sqrt(1 - MARKET_CORR * MARKET_CORR) * idio;

        double drift = (DRIFT - 0.5 * sigma * sigma) * DT;
        double diffusion = sigma * Math.sqrt(DT) * returnNoise;

        return currentPrice * Math.exp(drift + diffusion);
    }

    private Quote buildQuote(String symbol, double price, double change) {
        double spread = price * 0.0002; // 2bps spread
        return Quote.builder()
                .symbol(symbol)
                .price(price)
                .bid(price - spread / 2)
                .ask(price + spread / 2)
                .volume(Math.round(random.nextDouble() * 100_000))
                .change(change)
                .changePercent(price > 0 ? (change / (price - change)) * 100.0 : 0.0)
                .timestamp(Instant.now())
                .build();
    }

    private void broadcastQuotes() {
        messagingTemplate.convertAndSend("/topic/quotes",
                new ArrayList<>(currentQuotes.values()));
    }

    public Quote getQuote(String symbol) {
        return currentQuotes.get(symbol);
    }

    public List<Quote> getAllQuotes() {
        return new ArrayList<>(currentQuotes.values());
    }
}
