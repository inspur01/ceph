#!/bin/sh

replace_setfacl()
{
	OLD_TEXT=$2
	NEW_TEXT=$3
	
	if [ -z "$1" ];then
		echo "replace_setfacl: you must specify path"
		exit -1
	fi

	SETFACL_PATH=$1

	echo "replace [$2] to [$3]"

	cd $SETFACL_PATH
	FILES=`find -type f`

	for EACH in $FILES; do
		sed -i "s/$OLD_TEXT/$NEW_TEXT/g" $EACH

		#重命名文件名中含有setfacl的文件，将setfacl替换为setxacl
		OLD_FILE_0=`basename $EACH | grep -E "$OLD_TEXT"`
		if [ -n "$OLD_FILE_0" ]; then
			DIR=`dirname $EACH`
			OLD_FILE="$DIR/$OLD_FILE_0"
			
			NEW_FILE_0=`echo "$OLD_FILE_0" | sed "s/$OLD_TEXT/$NEW_TEXT/g"`
			NEW_FILE="$DIR/$NEW_FILE_0"

			mv "$OLD_FILE" "$NEW_FILE"
		fi
	done

	DIRS=`find ./ -type d`
	for EACH in $DIRS; do
		OLD_DIR_0=`basename $EACH | grep -E  "$OLD_TEXT"`
		if [ -n "$OLD_DIR_0" ]; then
			OLD_DIR_DIR=`dirname $EACH`
			OLD_OLD_DIR="${OLD_DIR_DIR}/${OLD_DIR_0}"
			
			NEW_DIR_0=`echo $OLD_DIR_0 | sed  "s/$OLD_TEXT/$NEW_TEXT/g"`
			NEW_DIR_DIR=`echo $OLD_DIR_DIR | sed "s/$OLD_TEXT/$NEW_TEXT/g"`
			
			#find -type d按照深度优先遍历所有的目录，所以父目录肯定在之前已经替换过setfacl关键字
			#这里需要替换的旧的全路径需要用替换后的父目录路径
			OLD_DIR="${NEW_DIR_DIR}/${OLD_DIR_0}"
			NEW_DIR="${NEW_DIR_DIR}/${NEW_DIR_0}"
			
			mv "$OLD_DIR" "$NEW_DIR"
		fi	
	done
	cd ..
}

if [ -n $1 ]; then 
	echo "start to replace key word"
	replace_setfacl $1 setfacl setxacl
	replace_setfacl $1 Setfacl Setxacl
	replace_setfacl $1 SETFACL SETXACL
	echo "done"
else
	echo "you must input the path"
fi
