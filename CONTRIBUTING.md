# Contributing to Commissioning Fabric

Commissioning Fabric is developed by Summon Software Labs and is licensed under
the Apache License, Version 2.0. Contributions are accepted under the same
license.

## Contribution terms

By submitting a contribution you agree that it is your original work, or that
you have the right to submit it, and that it is licensed to the project under the
Apache License, Version 2.0 (inbound equals outbound). There is no Contributor
License Agreement to sign and no copyright assignment: you keep the copyright to
your contribution, and the project distributes it under the Apache License,
Version 2.0.

Do not add co-author trailers or attribution lines to commits. Do not add
generated files, editor configuration, build output or machine-specific paths to
the repository.

## What a change must satisfy

1. It builds cleanly in Release and Debug with the first-party warning policy
   this repository sets (MSVC /W4 /WX, GCC/Clang -Wall -Wextra -Werror and the
   rest of the configured set). Warnings are not suppressed; they are fixed.
2. It adds or updates tests for the behaviour it introduces. Tests are proof
   obligations: a change to the lifecycle, persistence or authority model without
   a test that would fail before the change is incomplete.
3. It keeps the doctrine: observation is not authority, acknowledgement is not
   effect, missing or unknown is never converted to satisfied, stale authority is
   fenced rather than inherited, and no canonical record owned by an adjacent
   registry is created or mutated by this runtime.
4. It stays dependency-light. A new third-party dependency needs a compelling
   systems reason and a note in the pull request.
5. It keeps durable formats versioned and integrity-checked. A format change
   increments the format version and documents the migration.
6. It does not transmit telemetry, contact the network, or read machine
   identification.
7. Documentation describes behaviour that is implemented and verified, not
   behaviour that is intended.

## Building and testing

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

The same commands work with the Debug, RelWithDebInfo and sanitizer
configurations described in the README. Run the install/export and downstream
consumer validation described in the README before proposing a change that
touches the public headers or the CMake package.

## Reporting defects

A useful report contains the exact command, the store directory contents (or the
command sequence that produced them), the observed output, the expected
behaviour, and the runtime's reason code. Reason codes are stable and are listed
in the README.
