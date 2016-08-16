#! /bin/sh
if [ $# != 1 ]
then
    echo "please input poolname"
    exit
fi
poolname=$1
ceph df |grep $poolname
if [ $? -ne 0 ]
then
    echo "$poolname is not exist"
    exit
fi
mkdir -p /var/log/ceph/reweight_log
poolid=`ceph df |grep "$poolname"|awk '{print $2}'`
time=`date +%Y%m%d%H%M%S`
loop=0
while [ 1 ]
do
    while [ 1 ]
    do
        sleep 15
        ceph -s |grep "HEALTH_OK"
        if [ $? -eq 0 ]
        then
            break
        fi
    done
    pg_stat_file="/var/log/ceph/reweight_log/pg_stat_${time}_${loop}"
    max_sum_file="/var/log/ceph/reweight_log/max_sum_${time}_${loop}"
    osd_reweight_file="/var/log/ceph/reweight_log/osd_reweight_${time}_${loop}"
    ceph pg dump |grep -E "^pg_stat|^$poolid"| awk '
     /^pg_stat/ { col=1; while($col!="up") {col++}; col++ }
     /^[0-9a-f]+\.[0-9a-f]+/ { match($0,/^[0-9a-f]+/); pool=substr($0, RSTART, RLENGTH); poollist[pool]=0;
     up=$col; i=0; RSTART=0; RLENGTH=0; delete osds; while(match(up,/[0-9]+/)>0) { osds[++i]=substr(up,RSTART,RLENGTH); up = substr(up, RSTART+RLENGTH) }
     for(i in osds) {array[osds[i],pool]++; osdlist[osds[i]];}
    }
    END {
     printf("\n");
     printf("pool :\t"); for (i in poollist) printf("%s\t",i); printf("| SUM \n");
     for (i in poollist) printf("--------"); printf("----------------\n");
     for (i in osdlist) { printf("osd.%i\t", i); sum=0;
       for (j in poollist) { printf("%i\t", array[i,j]); sum+=array[i,j]; sumpool[j]+=array[i,j] }; printf("| %i\n",sum) }
     for (i in poollist) printf("--------"); printf("----------------\n");
     printf("SUM :\t"); for (i in poollist) printf("%s\t",sumpool[i]); printf("|\n");
    }' > $pg_stat_file
    cat $pg_stat_file |grep osd|awk 'BEGIN{max=0;osd_cnt=0;sum=0} {if($2>max) max=$2; osd_cnt++;sum+=$2} END{printf("%s %s %s\n", max,osd_cnt,sum)}'>$max_sum_file
    max=`cat $max_sum_file |awk '{print $1}'`
    osd_cnt=`cat $max_sum_file |awk '{print $2}'`
    sum=`cat $max_sum_file |awk '{print $3}'`
    overload=`awk 'BEGIN{printf "%.2f\n", ('$sum'/'$osd_cnt')*104/100}'`
    if [ `echo "$max < $overload" |bc` -eq 1 ]
    then
        break
    fi
    ceph osd reweight-by-pg 100 $poolname > $osd_reweight_file
    let loop=loop+1
    if [ $loop -gt 15 ]
    then
        break
    fi
done

