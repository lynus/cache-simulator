#!/bin/sh
for trace_file in `ls ~/trace/cello92/*.trace`
do
	echo $trace_file >/tmp/progress
	num=`echo $trace_file |grep '[0-9]*\.ds' -o|grep '[0-9]*' -o `
	./cut $trace_file > ${num}.trace
	lines=`wc -l ${num}.trace|cut -f1 -d' '`
	lines=`expr $lines - 10`
	./cache_sim -t ${num}.trace -r /tmp/save_rule -l $lines -c 12 >>result
	echo -e '\n'; echo -e '\n';echo -e '\n';echo -e '\n'
done
