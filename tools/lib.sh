# lib.sh — what every tool in tools/ shares. Sourced, never run: a tool
# sources it before it changes directory, and after `set -euo pipefail`.
#
#   tools_dir          this folder, absolute
#   tool_file          the tool that sourced it, absolute
#   repo_root          the checkout it belongs to
#   use_venv           meson and ninja on PATH: from the repo's .venv unless
#                      both already run there; venv_bin is the bin dir added,
#                      empty when none was
#   die <message...>   "<tool>: <message>" on stderr, exit status 2
#   usage_from_header  the calling tool's leading comment block, the help text

tools_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
tool_file=$(cd -- "$(dirname -- "${BASH_SOURCE[1]}")" && printf '%s/%s' "$(pwd -P)" "${BASH_SOURCE[1]##*/}")

repo_root() {
	printf '%s\n' "${tools_dir%/*}"
}

use_venv() {
	venv_bin=$("$tools_dir/venv.sh" --if-needed)
	if [ -n "$venv_bin" ]; then
		PATH=$venv_bin:$PATH
		export PATH
	fi
}

die() {
	echo "${0##*/}: $*" >&2
	exit 2
}

usage_from_header() {
	awk 'NR > 1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "$tool_file"
}
