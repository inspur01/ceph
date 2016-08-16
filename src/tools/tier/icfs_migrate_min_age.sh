#/bin/bash
function help()
{
    echo "Usage: icfs_migrate_min_age -c <cachepool> -t <time>"
	echo "  -c <cachepool> cachepool name to migrate "
    echo "  -t <time> min time of object staying in cachepool; it must be merasured in s(econd), m(inute), h(our) or d(ay)"
	echo "            For example, 120s, 4m, 2h or 1d."
    echo "Help options:"
    echo "  -h|? :Show this help message"
}

good=0

while getopts c:t:h option
do
	case "$option" in
		c)	
			type1="poolname"
            poolname=$OPTARG			
			;;
		t)	
			type2="time"
            time=$OPTARG			
			;;					
		h|?)
			help
			exit 1
			;;
	esac
done

if [ "$type1" == "poolname" ] && [ "$type2" == "time" ]  ; then
        good=1
        pool=`icfs osd dump|awk 'BEGIN{FS=" "} {print $3}' |grep \'$poolname\'`
        if [ "" != "$pool" ]; then
		lenth=${#time}
                if [ $lenth -le 1 ]; then
                    help
                    exit 1
                fi
		merasure=${time:$lenth-1}
		seconds=${time:0:lenth-1}
		end=`echo $seconds|grep -E '[^0-9]'`
        if [ "$end" == "$seconds" ] || [ $seconds -lt 0 ] ; then
                printf "Invalid value before time unit!\n\n"
                help
                exit 1
        fi
		if [ "$merasure" == "s" ] ; then
		    let min_age=$seconds
		elif [ "$merasure" == "m" ] ; then
		        let min_age=$seconds*60
		     elif [ "$merasure" == "h" ] ; then
			           let min_age=$seconds*60*60
			      elif [ "$merasure" == "d" ] ; then
				           let min_age=$seconds*60*60*24
						else 
						   help
						   exit 1
		fi
#                icfs osd tier tier_min_acccess_age $poolname $min_age
#		icfs osd pool set $poolname tier_min_access_age $min_age
		icfs osd pool set $poolname hit_set_count 2  > /dev/null 2>&1
		icfs osd pool set $poolname hit_set_type bloom   > /dev/null 2>&1
		let flush_age=$min_age/3
		icfs osd pool set $poolname hit_set_period $flush_age  > /dev/null 2>&1
                icfs osd pool set $poolname cache_min_flush_age $min_age  #10  > /dev/null 2>&1
              #  icfs osd pool set $poolname tier_min_access_age $min_age
        else
                printf "Pool $poolname doesn't exist. Please create it to continue.\n\n"
                help
                exit 1
        fi
fi
if [ $good == 0 ] ; then
        help
	exit 1
fi
