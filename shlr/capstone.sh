#!/bin/sh
CS_URL="$1" # url
CS_BRA="$2" # branch name
CS_TIP="$3" # tip commit
CS_REV="$4" # revert
CS_DEPTH_CLONE=10

git --help > /dev/null 2>&1
if [ $? != 0 ]; then
	echo "ERROR: Cannot find git command in PATH"
	exit 1
fi

fatal_msg() {
	echo "[capstone] $1"
	exit 1
}

patch_capstone() {
	echo "[capstone] Applying patches..."
	for patchfile in ../capstone-patches/*.patch ; do
		yes n | patch -p 1 -i "${patchfile}"
	done
}

parse_capstone_tip() {
	if [ -n "${CS_REV}" ]; then
		HEAD="$(git rev-parse --verify HEAD^)"
	else
		HEAD="$(git rev-parse --verify HEAD)"
	fi
	BRANCH="$(git rev-parse --abbrev-ref HEAD)"
}

clone_capstone() {
	if [ ! -d capstone ]; then
		git clone --quiet --single-branch --branch "${CS_BRA}" \
		    --depth "$CS_DEPTH_CLONE" "${CS_URL}" capstone \
		  || fatal_msg 'Cannot clone capstone from git'
	fi
	cd capstone && parse_capstone_tip
	cd - || fatal_msg 'Cannot change working directory'
}

update_capstone_git() {
	git checkout "${CS_BRA}" || fatal_msg "Cannot checkout to branch $CS_BRA"
#	if [ -n "${CS_TIP}" ]; then
#		# if our shallow clone not contains CS_TIP, clone until it
#		# contain that commit.
#		cur_depth="$CS_DEPTH_CLONE"
#		until git cat-file -e "${CS_TIP}^${commit}"; do
#			cur_depth=$(( cur_depth + 10 ))
#			git pull --depth="$cur_depth"
#		done
#		git reset --hard "${CS_TIP}"
#	fi
	git reset --hard "${CS_TIP}"
	if [ -n "${CS_REV}" ]; then
		if ! git config user.name ; then
			git config user.name "radare-travis"
			git config user.email "radare-travis@foo.com"
		fi
		env EDITOR=cat git revert --no-edit "${CS_REV}"
	fi
	return 0
}

if [ -d capstone ] && [ ! -d capstone/.git ]; then
	printf '[capstone] release with no git?\n' >&2
	cd capstone || fatal_msg 'Cannot change working directory'
	#patch_capstone
	cd -
else
	clone_capstone

	if [ "${BRANCH}" != "${CS_BRA}" ]; then
		echo '[capstone] Reset capstone' >&2
		rm -rf capstone
		clone_capstone
	fi

#	if [ "${HEAD}" = "${CS_TIP}" ]; then
#		printf '[capstone] Already in TIP, no need to update from git\n' >&2
#		git reset --hard
#		exit 0
#	fi

	echo '[capstone] Updating capstone from git...' >&2
	echo "HEAD ${HEAD}" >&2
	echo "TIP  ${CS_TIP}" >&2

	cd capstone || fatal_msg 'Cannot change working directory'
	update_capstone_git
	#patch_capstone
	cd -
fi
