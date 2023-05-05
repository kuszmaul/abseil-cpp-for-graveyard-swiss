cc_library(
    name = "flat_hash_set",
    hdrs = ["flat_hash_set.h"],
    deps = [
        "//absl/container:container_memory",
        "//absl/container:hash_function_defaults",
        "//absl/container:raw_hash_set",
        "//absl/algorithm:container",
        "//absl/base:core_headers",
        "//absl/memory",
    ],
)

cc_test(
    name = "flat_hash_set_test",
    srcs = ["flat_hash_set_test.cc"],
    deps = [
        ":flat_hash_set",
        "//absl/container:hash_generator_testing",
        "//absl/container:unordered_set_constructor_test",
        "//absl/container:unordered_set_lookup_test",
        "//absl/container:unordered_set_members_test",
        "//absl/container:unordered_set_modifiers_test",
        "//absl/base:raw_logging_internal",
        "//absl/memory",
        "//absl/strings",
        "@com_google_googletest//:gtest_main",
    ],
)
