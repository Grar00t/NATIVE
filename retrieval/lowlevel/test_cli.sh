#!/bin/sh
set -eu
cli=$1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cd "$dir"
reject() {
    if "$@" >out 2>err; then echo "unexpected success: $*" >&2; exit 1; fi
}
printf 'local\tفهرس\tfile\t18446744073709551615\t4294967295\tsha\tالبحث المحلي\n' > valid
"$cli" index a.dalw valid
"$cli" verify a.dalw
"$cli" search a.dalw 1 البحث المحلي > hit
grep -q '"title":"فهرس"' hit
grep -q '"offset":18446744073709551615,"length":4294967295' hit
"$cli" search a.dalw,a.dalw 2000000000 البحث > two
test "$(wc -l < two)" -eq 2
for k in 0 -1 1x abc 18446744073709551616; do reject "$cli" search a.dalw "$k" البحث; done
for paths in '' , a.dalw, ,a.dalw; do reject "$cli" search "$paths" 1 البحث; done
paths=$(awk 'BEGIN{for(i=0;i<64;i++)printf "%sa.dalw",i?",":""}')
"$cli" search "$paths" 1 البحث > one
test "$(wc -l < one)" -eq 1
reject "$cli" search "$paths,a.dalw" 1 البحث
reject "$cli" index bad.dalw .
test ! -e bad.dalw
for field in '1x 0' '18446744073709551616 0' '0 -1' '0 4294967296'; do
    set -- $field
    cp valid rows
    printf 'u\tt\tf\t%s\t%s\td\tx\n' "$1" "$2" >> rows
    reject "$cli" index bad.dalw rows
    test ! -e bad.dalw
    grep -q 'rows:2:' err
done
printf 'u\tt\tf\t0\t0\td\tx\000tail\n' >> rows
"$cli" index mixed.dalw rows --skip-invalid
"$cli" info mixed.dalw | grep -q '^docs=1 '
: > empty
"$cli" index empty.dalw empty
"$cli" search empty.dalw 1 البحث >empty-result
test ! -s empty-result
sha256sum a.dalw >original
reject "$cli" index a.dalw valid
sha256sum -c original
awk 'BEGIN{printf "local\t";for(i=0;i<20000;i++)printf "a";print "\tfile\t0\t3\tsha\tالبحث"}' >large
"$cli" index large.dalw large
"$cli" search large.dalw 1 البحث >large.json
test "$(wc -l <large.json)" -eq 1
awk -F'"title":"' 'NF==2 {split($2,a,"\"");if(length(a[1])==20000)ok=1} END{exit !ok}' large.json
printf 'local\traw_\355\240\200\tfile\t0\t3\tsha\tالبحث\n' > invalid-utf8
"$cli" index unicode.dalw invalid-utf8
"$cli" search unicode.dalw 1 البحث > unicode-result
grep -Fq '"title":"raw_???"' unicode-result
DALW_EXACT=0 "$cli" search a.dalw 1 البحث المحلي > stem
cmp hit stem
DALW_EXACT=1 "$cli" search a.dalw 1 البحث المحلي > exact
if cmp -s stem exact; then echo 'expected distinct Arabic term hashes' >&2; exit 1; fi
pin=$("$cli" verify a.dalw | sed 's/.*sha256=//')
"$cli" verify a.dalw "$pin"
"$cli" verify a.dalw "$(printf '%s' "$pin" | tr a-f A-F)"
reject "$cli" verify empty.dalw "$pin"
cp a.dalw damaged.dalw
printf 'z' >> damaged.dalw
reject "$cli" verify damaged.dalw "$pin"
reject "$cli" verify a.dalw "$(printf '%064d' 0)"
reject "$cli" verify a.dalw "${pin%?}z"
reject "$cli" verify a.dalw "${pin%?}"
"$cli" search a.dalw 5 البحث غائب > none
test ! -s none
"$cli" search-any a.dalw,a.dalw 5 البحث البحث غائب > partial
test "$(wc -l <partial)" -eq 2
grep -Eq '"match":"any","matched_terms":\["[0-9a-f]{16}"\]' partial
while IFS= read -r receipt; do
    prefix=$(printf '%s' "$receipt" | sed 's/,"id":"[0-9a-f]*"}$//')
    hash=$(printf '%s' "$prefix" | sha256sum)
    test "$receipt" = "$(printf '%s,"id":"%s"}' "$prefix" "${hash%% *}")"
done < partial
"$cli" search-any a.dalw 5 مفقود غائب > none
test ! -s none
echo 'DALW CLI passed'
