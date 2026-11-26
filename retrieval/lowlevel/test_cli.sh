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
for mode in info verify; do
    if "$cli" "$mode" a.dalw >/dev/full 2>err; then
        echo "$mode reported success after a stdout write error" >&2
        exit 1
    fi
    grep -q '^stdout:' err
done
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
# Quote starts in one read buffer and ends in the next. Other copies lie outside the recorded range.
quote='abaaabمرحبا'
awk 'BEGIN{for(i=0;i<8188;i++)printf "x"}' >span
printf '%s-tail' "$quote" >>span
printf '%s' "$quote" >body
offset=$(wc -c <body)
cat span >>body
printf '%s' "$quote" >>body
length=$(wc -c <span)
digest=$(sha256sum <span)
printf 'local\tpassage\tunopened-metadata-path\t%s\t%s\t%s\tالبحث\n' "$offset" "$length" "${digest%% *}" > rows
"$cli" index receipts.dalw rows
quote_bytes=$(printf '%s' "$quote" | wc -c)
body_pin=$("$cli" verify receipts.dalw | sed 's/.*sha256=//')
expected=$(printf '{"shard":"%s","doc":0,"source_sha256":"%s","source_offset":%u,"source_length":%u,"offset":%u,"length":%u}' "$body_pin" "${digest%% *}" "$offset" "$length" "$((offset+8188))" "$quote_bytes")
test "$("$cli" cite receipts.dalw 0 body "$quote")" = "$expected"
reject "$cli" cite receipts.dalw 0 body ''
reject "$cli" cite receipts.dalw 1 body "$quote"
reject "$cli" cite receipts.dalw 0 body 'absent'
test ! -s out
cp body changed
printf '!' | dd of=changed bs=1 seek=$((offset+length-1)) conv=notrunc 2>/dev/null
reject "$cli" cite receipts.dalw 0 changed "$quote"
test ! -s out
reject "$cli" cite receipts.dalw 0 span "$quote"
reject "$cli" cite receipts.dalw 0 /no-such-source "$quote"
reject "$cli" cite a.dalw 0 body "$quote"

printf 'البحث المحلي\nغائب\n\n' >queries
"$cli" batch-match a.dalw --rows <queries >actual
"$cli" batch-match a.dalw <queries >postings
cmp actual postings
printf '[0]\n[]\n[]\n' >expected_rows
cmp actual expected_rows
"$cli" batch-match empty.dalw --rows <queries >empty_rows
printf '[]\n[]\n[]\n' >expected_rows
cmp empty_rows expected_rows
printf '\000bad\n' >bad_query
reject "$cli" batch-match a.dalw <bad_query
reject "$cli" batch-match a.dalw --bad
reject "$cli" batch-match damaged.dalw <queries
reject "$cli" duplicates a.dalw 4
reject "$cli" duplicates a.dalw -1
"$cli" duplicates empty.dalw >actual
test ! -s actual
printf 'u\tt\tf\t0\t0\td\t\nu\tt\tf\t0\t0\td\tzebra\n' > rows
"$cli" index features.dalw rows
"$cli" fingerprints features.dalw > printed
fp_pin=$("$cli" verify features.dalw | sed 's/.*sha256=//')
test "$(wc -l < printed)" -eq 3
grep -Fxq "{\"format\":\"dalw-simhash-v1\",\"shard\":\"$fp_pin\",\"documents\":2}" printed
grep -Fxq '{"doc":0,"simhash":null}' printed
grep -Eq '^\{"doc":1,"simhash":"[0-9a-f]{16}"\}$' printed
echo 'DALW CLI passed'
