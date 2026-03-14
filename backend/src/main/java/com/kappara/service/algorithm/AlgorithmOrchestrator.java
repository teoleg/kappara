package com.kappara.service.algorithm;

import com.kappara.model.AlgorithmStatus;
import lombok.RequiredArgsConstructor;
import org.springframework.messaging.simp.SimpMessagingTemplate;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Service;

import java.util.List;
import java.util.stream.Collectors;

/**
 * Runs all registered algorithms on every market tick and broadcasts
 * their statuses to the UI via WebSocket.
 */
@Service
@RequiredArgsConstructor
public class AlgorithmOrchestrator {

    private final List<TradingAlgorithm> algorithms;
    private final SimpMessagingTemplate  messagingTemplate;

    @Scheduled(fixedRate = 1000)
    public void tick() {
        algorithms.forEach(TradingAlgorithm::onTick);

        List<AlgorithmStatus> statuses = algorithms.stream()
                .map(TradingAlgorithm::getStatus)
                .filter(s -> s != null)
                .collect(Collectors.toList());

        messagingTemplate.convertAndSend("/topic/algorithms", statuses);
    }

    public List<AlgorithmStatus> getStatuses() {
        return algorithms.stream()
                .map(TradingAlgorithm::getStatus)
                .filter(s -> s != null)
                .collect(Collectors.toList());
    }
}
