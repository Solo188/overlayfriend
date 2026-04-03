package com.endfield.overlayassistant;

public interface IAiAssistant {
    String processInput(String userInput, int affinityTier);
    String getTimeGreeting(int hour);
}
