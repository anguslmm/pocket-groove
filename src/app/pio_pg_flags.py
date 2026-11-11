# PlatformIO extra script (pre:) for env:cardputer-adv -- not firmware source.
#
# Gives the portable library lib/pg the same warning set as src/ (build_src_flags
# only reaches src/). docs/ARCHITECTURE.md requires lib/pg to build clean with
# these, and -Wdouble-promotion is how an accidental double (software-emulated
# on the ESP32-S3) in DSP code gets noticed. The framework and the M5 libraries
# keep their own flags, so their warnings do not drown ours.
#
# A build middleware is used because the alternative, build flags in a
# lib/pg/library.json, would mean editing lib/pg (owned by other people).
Import("env")  # noqa: F821  (SCons injects it)

PG_WARNINGS = ["-Wall", "-Wextra", "-Wdouble-promotion", "-Wfloat-conversion"]


def _pg_flags(env, node):
    path = node.srcnode().get_path().replace("\\", "/")
    if "/lib/pg/" not in path and not path.startswith("lib/pg/"):
        return node
    return env.Object(node, CCFLAGS=env["CCFLAGS"] + PG_WARNINGS)


env.AddBuildMiddleware(_pg_flags)  # noqa: F821
