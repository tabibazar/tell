# Source before any idf.py use.
# The venv on this machine is py3.13 while `python3` is now 3.14, so export.sh
# must be pointed at the existing env explicitly.
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"
. "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
