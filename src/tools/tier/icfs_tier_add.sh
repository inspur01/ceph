#/bin/bash
function help()
{
    echo "Usage: icfs_tier_add -b <basepool> -c <cachepool2>"
    echo "  -b <basepool>  name of the base pool"
	echo "  -c <cachepool>  name of the cache pool"
    echo "Help options:"
    echo "  -h|? :Show this help message"
}

good=0

while getopts b:c:h option
do
	case "$option" in
		b)	
			type1="basepool"
            basepool=$OPTARG			
			;;		
        c)	
			type2="cachepool"
            cachepool=$OPTARG			
			;;					
		h|?)
			help
			exit 1
			;;
	esac
done

if [ "$type1" == "basepool" ] && [ "$type2" == "cachepool" ]; then
        good=1
        base=`icfs osd dump|awk 'BEGIN{FS=" "} {print $3}' |grep \'$basepool\'`
        cache=`icfs osd dump|awk 'BEGIN{FS=" "} {print $3}' |grep \'$cachepool\'`
#        echo $base
#        echo $cache
        if [ "" != "$base" ]  &&  [ "" != "$cache" ] ; then
                icfs osd tier add $basepool $cachepool
                icfs osd tier cache-mode $cachepool writeback  >/dev/null 2>&1
                icfs osd tier set-overlay $basepool $cachepool  
                #icfs mds add_data_pool $basepool >/dev/null 2>&1
 #               printf "Pool $basepool or $cachepool doesn't exist. Please create it to continue.\n\n"
  #              help
  #              exit 1
#        fi
#		icfs osd tier add $basepool $cachepool 
#		icfs osd tier cache-mode $cachepool writeback  >/dev/null 2>&1
#		icfs osd tier set-overlay $basepool $cachepool  
#		icfs mds add_data_pool $basepool >/dev/null 2>&1
           else
                printf "Pool $basepool or $cachepool doesn't exist. Please create it to continue.\n\n"
                help
                exit 1
       fi
#		if [ $? != 0 ] ; then
#	        printf "Failed\n!"
#		exit 1
#        fi
#        printf "OK!\n"
fi
if [ $good == 0 ] ; then
        help
	exit 1
fi
