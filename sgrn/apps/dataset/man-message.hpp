#pragma once

#include <string_view>

constexpr std::string_view MAN_MESSAGE = R"(SGRN_DATASET(1)                  User Commands                  SGRN_DATASET(1)

NAME
       sgrn_dataset - SGRN Industrial Telemetry Dataset Processor & Data Canonicalizer

SYNOPSIS
       sgrn_dataset -i FILE_OR_DIR [-o OUT_FILE] [-f FORMAT] [-s SCHEMA.scl]

DESCRIPTION
       sgrn_dataset is the C++ data canonicalization and conversion engine for SGRN.
       It processes, converts, compresses/decompresses, and canonicalizes telemetry archives.

OPTIONS
       -i, --input FILE|DIR
              Input archive or directory. Can also be passed positionally.

       -o, --output FILE
              Output destination file path. If omitted for conversion, infers format extension.

       -f, --format FORMAT
              Target format for conversion: binary, jsonl, or csv. Default: auto.

       -c, --convert FILE
              Shortcut to convert specified file.

       -s, --scl FILE.scl
              Path to SCL schema file (.scl) for processing datasets.
\n)";
