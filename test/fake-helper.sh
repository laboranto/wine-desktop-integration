#!/bin/sh
# 가짜 헬퍼: --title= 로 시나리오를 고르고, 받은 인자(--out= 값은 OUT)를 결과 폴더의 wpf-fake-args 에 기록
log= out= title=
for a; do
    case $a in
        --out=*) out=${a#--out=}; a=--out=OUT ;;
        --title=*) title=${a#--title=} ;;
    esac
    log="$log$a
"
done
dir=$(dirname "$out")
printf '%s' "$log" > "$dir/wpf-fake-args"
mkdir -p /tmp/wpf-test /tmp/wpf-test2 && touch /tmp/wpf-test/photo.jpg /tmp/wpf-test/a.txt /tmp/wpf-test/b.txt /tmp/wpf-test2/c.txt
case $title in
    single) printf '2\n/tmp/wpf-test/photo.jpg\n' > "$out" ;;
    multi) printf '0\n/tmp/wpf-test/a.txt\n/tmp/wpf-test/b.txt\n' > "$out" ;;
    mixed) printf '0\n/tmp/wpf-test/a.txt\n/tmp/wpf-test2/c.txt\n' > "$out" ;;
    save) printf '0\n/tmp/wpf-test/noext\n' > "$out" ;;
    overwrite)  # 첫 호출은 이미 있는 photo(.jpg), 두 번째는 noext
        if [ -e "$dir/wpf-fake-count" ]; then printf '0\n/tmp/wpf-test/noext\n' > "$out"
        else touch "$dir/wpf-fake-count"; printf '0\n/tmp/wpf-test/photo\n' > "$out"; fi ;;
    cancel) exit 10 ;;
    *) exit 2 ;;
esac
