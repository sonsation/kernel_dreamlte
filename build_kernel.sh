#!/bin/bash

export ARCH=arm64
DEFCONFIG_NAME=exynos8895-dreamltekor_kor_defconfig
RDIR=$(pwd)
IMAGE_DIR=$RDIR/arch/$ARCH/boot
DTS_DIR=$IMAGE_DIR/dts/exynos
DTB_DIR=$IMAGE_DIR/dtb
DTCTOOL_DIR=$RDIR/scripts/dtc/dtc
INC_DIR=$RDIR/include

model=none
PAGE_SIZE=2048
DTB_PADDING=0

DTSFILES="exynos8895-dreamlte_kor_05 
	exynos8895-dreamlte_kor_07
	exynos8895-dreamlte_kor_09
	exynos8895-dreamlte_kor_10
	exynos8895-dreamlte_kor_11"

export CROSS_COMPILE=$RDIR/tools/prebuilts/gcc-cfp-jopp-only/aarch64-linux-android-4.9/bin/aarch64-linux-android-
export ANDROID_MAJOR_VERSION=o
#export PATH=$(pwd)/./tools/prebuilts/gcc-cfp-jopp-only/aarch64-linux-android-4.9/bin:$PATH
export BUILD_JOB_NUMBER=`grep processor /proc/cpuinfo|wc -l`


func_make_dts() 
{

	[ -f "$DTCTOOL_DIR" ] || {
		echo "You need to run ./build.sh first!"
		exit 1
	}

	if [ ! -d $DTB_DIR ] 
	then
		mkdir $DTB_DIR
		
	fi

	cd $DTB_DIR || {
			echo "Unable to cd to $DTBDIR!"
			exit 1
		}
		
	for dts in $DTSFILES; do
		echo "=> Processing: ${dts}.dts"
		${CROSS_COMPILE}cpp -nostdinc -undef -x assembler-with-cpp -I "$INC_DIR" "$DTS_DIR/${dts}.dts" > "${dts}.dts"
		echo "=> Generating: ${dts}.dtb"
		$DTCTOOL_DIR -p $DTB_PADDING -i "$DTS_DIR" -O dtb -o "${dts}.dtb" "${dts}.dts"
	done

	echo "Generating dtb.img..."
	$RDIR/scripts/dtbTool/dtbTool -o "$IMAGE_DIR/dtb.img" -d "$DTB_DIR/" -s $PAGE_SIZE

	[ -f "$RDIR/ramdisk/$1/split_img/boot.img-zImage"] || {
		rm -f $RDIR/ramdisk/$model/split_img/boot.img-zImage
	}

	[ -f "$RDIR/ramdisk/$1/split_img/boot.img-dtb"] || {
		rm -f $RDIR/ramdisk/$model/split_img/boot.img-dtb
	}

	mv -f $IMAGE_DIR/Image $RDIR/ramdisk/$1/split_img/boot.img-zImage
        mv -f $IMAGE_DIR/dtb.img $RDIR/ramdisk/$1/split_img/boot.img-dtb
	cd $RDIR/ramdisk/$1
	./repackimg.sh --nosudo
	echo SEANDROIDENFORCE >> image-new.img	

	func_make_zip
}

func_make_zip()
{
	
	timestamp=`date +%Y%m%d%H%M`
	mv -f $RDIR/ramdisk/$model/image-new.img $RDIR/preZip/AU/boot.img
	
	cd $RDIR/preZip

	zip -r AU_${model}_${timestamp}.zip system AU META-INF

	if [ ! -d $RDIR/export ] 
	then
		mkdir $RDIR/export
		mv -f $RDIR/preZip/AU_${model}_${timestamp}.zip $RIDR/export
	fi

	
	
}

echo "________________________________"
echo "|                               |"
echo "|                               |"
echo "|     Anti Unstable V 1.0       |"
echo "|                               |"
echo "|_______________________________|"
echo "_____________CLEAN______________"
echo "0)make clean"
echo "___________SUPPORTED____________"
echo "1)Build Kernel for G950F (S/K/L)"
echo "2)Build Kernel for G950F (S/K/L) (clean)"
echo "_________NOT SUPPORTED__________"
echo "3)Build Kernel for G955F (S/K/L)"
echo "4)Build Kernel for G955F (S/K/L) (clean)"
echo "Enter a number : "

read NUM
case $NUM in
0) make clean
   exit 1
;;
1) eval model=G950F ;;
2) eval model=G950F
   make clean ;;
3) eval model=G955F ;;
4) eval model=G955F
   make clean ;;
*) echo "INVALID NUMBER! You must enter a number between 0 and 4." ;;
esac 
	
	rm -f $RDIR/arch/$ARCH/boot/dtb/*.dtb
	rm -f $RDIR/arch/$ARCH/boot/Image
	rm -f $RDIR/arch/$ARCH/boot/Image.gz
	rm -f $RDIR/arch/$ARCH/boot/dtb.img
	rm -f $RDIR/ramdisk/$model/split_img/boot.img-zImage
	rm -f $RDIR/ramdisk/$model/split_img/boot.img-dtb
	

	[ -f "$RDIR/.config" ] || {
		make $DEFCONFIG_NAME
		echo "Copy Config......"
	}
	
	make -j$BUILD_JOB_NUMBER	
	func_make_dts $model




