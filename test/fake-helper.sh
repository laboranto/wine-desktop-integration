#!/bin/sh
# 가짜 헬퍼: --title= 로 시나리오를 고르고, 받은 인자(--out= 값은 OUT)를 결과 폴더의 wpf-fake-args 에 기록
# 고른 파일은 시험 폴더 $WDI_TEST_DIR(run.sh 가 mktemp 로 만듦) 안에 둔다
# explorer.exe 시험: show-items|show-folders <경로> 를 $WDI_TEST_DIR/explorer-args 에 기록
d=${WDI_TEST_DIR:?}
case ${1:-} in show-items|show-folders) printf '%s\n' "$@" > "$d/explorer-args"; exit 0 ;; esac
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
mkdir -p "$d/2" && touch "$d/photo.jpg" "$d/a.txt" "$d/b.txt" "$d/2/c.txt"
case $title in
    single) printf '2\n%s/photo.jpg\n' "$d" > "$out" ;;
    multi) printf '0\n%s/a.txt\n%s/b.txt\n' "$d" "$d" > "$out" ;;
    mixed) printf '0\n%s/a.txt\n%s/2/c.txt\n' "$d" "$d" > "$out" ;;
    save) printf '0\n%s/noext\n' "$d" > "$out" ;;
    overwrite)  # 첫 호출은 이미 있는 photo(.jpg), 두 번째는 noext
        if [ -e "$dir/wpf-fake-count" ]; then printf '0\n%s/noext\n' "$d" > "$out"
        else touch "$dir/wpf-fake-count"; printf '0\n%s/photo\n' "$d" > "$out"; fi ;;
    cancel) exit 10 ;;
    *) exit 2 ;;
esac
