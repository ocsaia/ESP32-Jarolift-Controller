#pragma once

/*
 * mqtt.cpp includes <WiFi.h> and uses nothing from it - the connection is
 * managed in basics.cpp. Kept as an empty stand-in rather than removing the
 * include from the firmware, so the test build stays a pure observer of the
 * source it exercises.
 */
