find_package(GTest QUIET)

if(NOT GTest_FOUND)
    return()
endif()

add_executable(sync-core-test
    cloud_vehicle_sync_core_test.cpp
)

target_link_libraries(sync-core-test
    PRIVATE
        ifex-sync-core
        GTest::gtest
        GTest::gtest_main
)

add_test(NAME sync_core_duplicate_apply_test COMMAND sync-core-test --gtest_filter=SyncCoreTest.DuplicateApplyReplay)
add_test(NAME sync_core_dominated_remote_update_test COMMAND sync-core-test --gtest_filter=SyncCoreTest.DominatedRemoteUpdateIsStaleRejected)
add_test(NAME sync_core_conflict_concurrent_test COMMAND sync-core-test --gtest_filter=SyncCoreTest.ConcurrentConflictGeneratesDeterministicRecord)
add_test(NAME sync_core_tombstone_replay_test COMMAND sync-core-test --gtest_filter=SyncCoreTest.TombstoneReplayMarkedDuplicate)
add_test(NAME sync_core_ack_replay_test COMMAND sync-core-test --gtest_filter=SyncCoreTest.AckReplayDoesNotAdvanceCheckpoint)
add_test(NAME sync_core_gap_trigger_test COMMAND sync-core-test --gtest_filter=SyncCoreTest.GapRecoveryOnlyOnMismatchWithoutDirty)
add_test(NAME sync_core_non_owner_conflict_test COMMAND sync-core-test --gtest_filter=SyncCoreTest.NonOwnerMutationRejected)
