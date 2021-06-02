# helpers to generate command for debug
DATA_ADDR=`cat /sys/module/loop/sections/.data`
echo $DATA_ADDR

TEXT_ADDR=`cat /sys/module/loop/sections/.text`
echo $TEXT_ADDR

BSS_ADDR=`cat /sys/module/loop/sections/.bss`
echo $BSS_ADDR

CMD="add-symbol-file drivers/block/hyperblock/loop.ko "$TEXT_ADDR" -s .bss "$BSS_ADDR" -s .data "$DATA_ADDR  

echo "Will run: "$CMD"..."
echo $CMD > tmpcmd
