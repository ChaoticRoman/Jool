_jool_siit() {
	# Given the following incomplete command:
	#
	#     $ jool_siit global update m
	#
	# This function (_jool_siit()) gets called when the user hits tab.
	# It is expected to place the autocompletion candidates (in this
	# case, `manually-enabled` and `mtu-plateaus`) in the COMPREPLY variable.
	#
	#
	# COMP_WORDS is an array that contains all the tokens the user
	# has written so far. (Hence `(jool_siit global update m)`.)
	# COMP_CWORD is the index of the "current" token. (Hence `3`.)
	#
	# `compgen -W <a> -- <b>` prints the elements from `a` that
	# match the `b` prefix.
	#
	# "{$ARRAY[*]:1}" means "all of the elements of the array,
	# except for the first one."
	#
	# I got all of this from https://debian-administration.org/article/317/An_introduction_to_bash_completion_part_2
	COMPREPLY=($(compgen -W "$(${COMP_WORDS[0]} autocomplete ${COMP_WORDS[*]:1})" -- ${COMP_WORDS[COMP_CWORD]}))
}

complete -F _jool_siit jool_siit
