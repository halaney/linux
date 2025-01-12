#!/bin/bash

# Outputs a ready to use commit message for dist-git
# $1: spec file
# $2: zstream flag

spec=$1;
zstream_flag=$2;
package_name=$3;
rtbz=$4;
tmp=$(mktemp);

function die
{
	echo "Error: $1" >&2;
	rm -f "$tmp";
	exit 1;
}

if [ ! -f "$spec" ]; then
	die "$0 <spec file>";
fi

# this expression looks for the beginning of the changelog and extracts the
# first entry
sed -n -e '0,/%changelog/d; :abc; s/^$//; t end; p; d; N; b abc; :end; q;' "$spec" > "$tmp";
if [ ! -s "$tmp" ]; then
	die "Unable to extract the changelog";
fi

# getting the version from the first line
version=$(head -n 1 "$tmp" | sed -e 's/.*\[\(.*\)\]/\1/');
if [ -z "$version" ]; then
	die "Unable to get version/release";
fi

# extracting the BZs to create the "Resolves" line
if [ "$zstream_flag" == "no" ]; then
	bzs=$(grep ^- "$tmp" |
		sed -n -e "s/.*\[\([0-9 ]\+\)\].*/\1/p")
else
	bzs=$(awk '/^-/ {
		if(match($0, /\[([0-9]+ [0-9]+ )*[0-9]+ [0-9]+\]/)) {
			n = split(substr($0, RSTART + 1, RLENGTH - 2), bzs)
			for(i = 1; i <= n/2; i++)
				print bzs[i]
		}
	}' "$tmp")
fi

bzs="$rtbz $bzs"

echo $bzs |
	tr ' ' '\n' |
	sort -u |
	tr '\n' ',' |
	sed -e "s/^/Resolves: rhbz#/; s/,\$//; s/,/, rhbz#/g;" >> "$tmp";

echo -e "${package_name}-${version}\n"
cat "$tmp";
echo;
rm -f "$tmp";
