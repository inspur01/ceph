#/bin/bash
function help()
{
    echo "Usage: icfs_ratio_migrate -c <cachepool> -r <ratio>"
    echo "  -c <cachepool>  name of the cache pool"
    echo "  -r <ratio> percentage of target to migrate in earnest"
    echo "             and it is integer and between 40 and 70"
    echo "Help options:"
    echo "  -h|? :Show this help message"
}

good=0
while getopts c:r:h option
do
	case "$option" in
		  c)	
			    type1="cachepool"
          cachepool=$OPTARG			
			    ;;		
      r)	
			    type2="ratio"
          ratio=$OPTARG			
			    ;;					
		  h|?)
			    help
			    exit 1
			    ;;
  esac
done

interval=10
if [ "$type1" == "cachepool" ] && [ "$type2" == "ratio" ]; 
    then
        good=1
            cache=`icfs osd dump|awk 'BEGIN{FS=" "} {print $3}' |grep \'$cachepool\'`
#           echo $cache
            let full=$[$ratio+$interval]
#            echo full=$full
            if [ "" != "$cache" -a 40 -lt $ratio -a 70 -gt $ratio ] ; then 
                printf "Setting the fraction of target to begin migrate.....\n"
                ratio_dirty=`echo $ratio*0.01|bc`
                ratio_full=`echo $full*0.01|bc`
#                echo " ratio_dirty:"
#                echo $ratio_dirty
#                echo " ratio_full:"
#                echo $ratio_full
                icfs osd pool set $cachepool cache_target_dirty_ratio $ratio_dirty
                icfs osd pool set $cachepool cache_target_full_ratio $ratio_full

           else
                printf "Pool [$cachepool] doesn't exist or ratio [$ratio] is invalid. Please confirm them.\n\n"
                help
                exit 1
       fi
		if [ $? != 0 ] ; then
	        printf "Failed\n!"
		exit 1
        fi
        printf "OK!\n"
fi
if [ $good == 0 ] ; then
        help
	exit 1
fi
