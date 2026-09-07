#pragma once

/*
 * Stands in for include/basics.h during native tests.
 *
 * The real header pulls in WiFi, HTTPClient, mDNS and SPI, none of which
 * config.cpp uses - it needs muTimer for its change-detection timer and
 * EspStrUtil for the JSON string reads, the djb2 hash and the password
 * encryption. Both of those are the real libraries here, not fakes: the tests
 * exist to check what config.cpp does with them, so faking them would leave
 * nothing worth checking.
 *
 * This file shadows the real one because -I test/shim comes first on the
 * include path. If a module under test starts needing something from the real
 * basics.h, add it here deliberately rather than widening the include path.
 */

#include <ArduinoJson.h>
#include <EspStrUtil.h>  // the shim next to this file, not the library
#include <config.h>
#include <muTimer.h>
