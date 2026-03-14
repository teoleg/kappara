package com.kappara.controller;

import com.kappara.model.*;
import com.kappara.service.MarketDataService;
import com.kappara.service.RiskEngine;
import com.kappara.service.TradingEngine;
import com.kappara.service.algorithm.AlgorithmOrchestrator;
import lombok.RequiredArgsConstructor;
import org.springframework.web.bind.annotation.*;

import java.util.List;
import java.util.Map;

@RestController
@RequestMapping("/api")
@RequiredArgsConstructor
@CrossOrigin
public class TradingApiController {

    private final MarketDataService    marketDataService;
    private final TradingEngine        tradingEngine;
    private final RiskEngine           riskEngine;
    private final AlgorithmOrchestrator orchestrator;

    @GetMapping("/quotes")
    public List<Quote> quotes() {
        return marketDataService.getAllQuotes();
    }

    @GetMapping("/positions")
    public List<Position> positions() {
        return tradingEngine.getOpenPositions();
    }

    @GetMapping("/trades")
    public List<Trade> trades() {
        return List.copyOf(tradingEngine.getTradeLog());
    }

    @GetMapping("/risk")
    public RiskMetrics risk() {
        return riskEngine.getLatestMetrics();
    }

    @GetMapping("/algorithms")
    public List<AlgorithmStatus> algorithms() {
        return orchestrator.getStatuses();
    }

    @GetMapping("/instruments")
    public Map<String, Instrument> instruments() {
        return marketDataService.getInstruments();
    }
}
