#include <signal.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <applibs/log.h>
#include <applibs/gpio.h>

#include "epoll_timerfd_utilities.h"

static volatile sig_atomic_t terminationRequested = false;

static int epollFd = 0;

static int switchButtonFd = 0;
static int exitButtonFd = 0;

static int redLedFd = 0;
static int greenLedFd = 0;
static int blueLedFd = 0;

static int switchButtonTimerFd = 0;
static int exitButtonTimerFd = 0;

static int currentLed = 0;

static GPIO_Value_Type oldSwitchButtonState = GPIO_Value_High;
static GPIO_Value_Type oldExitButtonState = GPIO_Value_High;

static int leds[3];

static void TerminationHandler(int signalNumber);
static void SwitchButtonPollTimerEventHandler(EventData* eventData);
static int InitPeripheralsAndHandlers(void);
static void ClosePeripheralsAndHandlers(void);

int main(void)
{
    Log_Debug("Starting application");

    if (InitPeripheralsAndHandlers() != 0) {
        terminationRequested = true;
    }

    while (!terminationRequested) {
        if (WaitForEventAndCallHandler(epollFd) != 0) {
            terminationRequested = true;
        }
    }

    ClosePeripheralsAndHandlers();

    Log_Debug("Application exiting.\n");

    return 0;
}

static void TerminationHandler(int signalNumber)
{
    // Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
    terminationRequested = true;
}

static void SwitchButtonPollTimerEventHandler(EventData* eventData)
{
    if (ConsumeTimerFdEvent(switchButtonTimerFd) != 0) {
        terminationRequested = true;
        return;
    }

    GPIO_Value_Type value;
    int state = GPIO_GetValue(switchButtonFd, &value);
    if (state != 0)
    {
        Log_Debug("Unable to get button value");

        terminationRequested = true;
        return;
    }

    if (value != oldSwitchButtonState)
    {
        oldSwitchButtonState = value;
    }
    else
    {
        return;
    }

    if (value == GPIO_Value_Low)
    {
        GPIO_SetValue(leds[currentLed], GPIO_Value_High);

        ++currentLed;
        currentLed = currentLed % 3;

        GPIO_SetValue(leds[currentLed], GPIO_Value_Low);

        switch (currentLed)
        {
            case 0:
                Log_Debug("RED");
                break;
            case 1:
                Log_Debug("GREEN");
                break;
            case 2:
                Log_Debug("BLUE");
                break;
        }
    }
}

static void ExitButtonPollTimerEventHandler(EventData* eventData)
{
    if (ConsumeTimerFdEvent(exitButtonTimerFd) != 0) {
        terminationRequested = true;
        return;
    }

    GPIO_Value_Type value;
    int state = GPIO_GetValue(exitButtonFd, &value);
    if (state != 0)
    {
        Log_Debug("Unable to get button value");

        terminationRequested = true;
        return;
    }

    if (value != oldExitButtonState)
    {
        oldExitButtonState = value;
    }
    else
    {
        return;
    }

    if (value == GPIO_Value_Low)
    {
        Log_Debug("Terminating app\n");
        terminationRequested = true;
    }
}

static EventData switchButtonPollEventData = { .eventHandler = &SwitchButtonPollTimerEventHandler };
static EventData exitButtonPollEventData = { .eventHandler = &ExitButtonPollTimerEventHandler };

static int InitPeripheralsAndHandlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = TerminationHandler;
    sigaction(SIGTERM, &action, NULL);

    epollFd = CreateEpollFd();
    if (epollFd < 0) {
        return -1;
    }

    // Open button A GPIO as input
    Log_Debug("Opening button A as input\n");
    switchButtonFd = GPIO_OpenAsInput(12);
    if (switchButtonFd < 0) {
        Log_Debug("ERROR: Could not open button A: %s (%d).\n", strerror(errno), errno);
        return -1;
    }

    // Open button B GPIO as input
    Log_Debug("Opening button B as input\n");
    exitButtonFd = GPIO_OpenAsInput(13);
    if (exitButtonFd < 0) {
        Log_Debug("ERROR: Could not open button B: %s (%d).\n", strerror(errno), errno);
        return -1;
    }

    // LED 4 Blue is used to show Device Twin settings state
    Log_Debug("Opening SAMPLE_LED as output\n");
    redLedFd =
        GPIO_OpenAsOutput(8, GPIO_OutputMode_PushPull, GPIO_Value_Low);
    if (redLedFd < 0) {
        Log_Debug("ERROR: Could not open LED: %s (%d).\n", strerror(errno), errno);
        return -1;
    }

    greenLedFd =
        GPIO_OpenAsOutput(9, GPIO_OutputMode_PushPull, GPIO_Value_High);
    if (greenLedFd < 0) {
        Log_Debug("ERROR: Could not open LED: %s (%d).\n", strerror(errno), errno);
        return -1;
    }

    blueLedFd =
        GPIO_OpenAsOutput(10, GPIO_OutputMode_PushPull, GPIO_Value_High);
    if (blueLedFd < 0) {
        Log_Debug("ERROR: Could not open LED: %s (%d).\n", strerror(errno), errno);
        return -1;
    }

    leds[0] = redLedFd;
    leds[1] = greenLedFd;
    leds[2] = blueLedFd;

    // Set up a timer to poll for button events.
    struct timespec buttonPressCheckPeriod = { 0, 1000 * 1000 };
    switchButtonTimerFd =
        CreateTimerFdAndAddToEpoll(epollFd, &buttonPressCheckPeriod, &switchButtonPollEventData, EPOLLIN);
    if (switchButtonTimerFd < 0) {
        return -1;
    }

    exitButtonTimerFd =
        CreateTimerFdAndAddToEpoll(epollFd, &buttonPressCheckPeriod, &exitButtonPollEventData, EPOLLIN);
    if (exitButtonTimerFd < 0) {
        return -1;
    }

    return 0;
}

static void ClosePeripheralsAndHandlers(void)
{
    Log_Debug("Closing file descriptors\n");

    for (int i = 0; i < sizeof(leds) / sizeof(leds[0]); ++i)
    {
        CloseFdAndPrintError(leds[i], "LED");
    }

    CloseFdAndPrintError(switchButtonTimerFd, "SwitchButtonTimer");
    CloseFdAndPrintError(exitButtonTimerFd, "ExitButtonTimer");
    CloseFdAndPrintError(switchButtonFd, "ToggleLedButton");
    CloseFdAndPrintError(exitButtonFd, "ExitButton");
    CloseFdAndPrintError(epollFd, "Epoll");
}
