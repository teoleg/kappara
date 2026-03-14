package com.kappara;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.scheduling.annotation.EnableScheduling;

@SpringBootApplication
@EnableScheduling
public class KapparaApplication {
    public static void main(String[] args) {
        SpringApplication.run(KapparaApplication.class, args);
    }
}
