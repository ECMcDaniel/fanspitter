# Stamps the firmware with `git describe` as FW_VERSION (shown in the web UI,
# /api/status and the MQTT status topic). Applied to project sources only so
# a new commit doesn't force the framework and libraries to recompile.
import subprocess

Import("env", "projenv")  # noqa: F821 (provided by PlatformIO/SCons)


def git_version():
    try:
        return (
            subprocess.check_output(
                ["git", "describe", "--tags", "--always", "--dirty"],
                cwd=env.subst("$PROJECT_DIR"),  # noqa: F821
                stderr=subprocess.DEVNULL,
            )
            .decode()
            .strip()
        )
    except Exception:
        return "unknown"


version = git_version()
print("FanSpitter firmware version: " + version)
projenv.Append(CPPDEFINES=[("FW_VERSION", '\\"%s\\"' % version)])  # noqa: F821
