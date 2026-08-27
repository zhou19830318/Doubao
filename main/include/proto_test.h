/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * proto_test — temporary on-target self-test for the doubao protocol codec.
 * Registers the `proto test` console command (Task 5 Step 4, M2; 可保留至
 * M5 再删). Run:  proto test  → prints "proto test: PASS/FAIL".
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void proto_test_register(void);

#ifdef __cplusplus
}
#endif
