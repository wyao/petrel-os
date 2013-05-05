# dirconc tests
echo ">>>>    starting test 1    >>>>"
cd ~/cs161/root; sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 5; p /testbin/dirconc lhd1: 5'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 2    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 20; p /testbin/dirconc lhd1: 20'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 3    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 40; p /testbin/dirconc lhd1: 40'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
# fs1 test
echo ">>>>    starting test 4    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 1; fs1 lhd1:'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 5    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 20; fs1 lhd1:'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 6    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 40; fs1 lhd1:'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
# fs2 tests
echo ">>>>    starting test 7    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 10; fs2 lhd1:'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 8    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 20; fs2 lhd1:'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
# fs3 tests
echo ">>>>    starting test 9    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 10; fs3 lhd1:'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 10    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 50; fs3 lhd1:'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 11    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 100; fs3 lhd1:'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='

echo ">>>>    starting test 12    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 5; p /testbin/dirseek'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 13    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 5; p /testbin/bigfile test.txt 10000'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 14    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 5; p /testbin/dirtest'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 15    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 5; p /testbin/rmdirtest'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 16    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 2; p /testbin/rmtest'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 17    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 100; p /testbin/f_test'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='
echo ">>>>    starting test 18    >>>>"
sys161 kernel 'p /sbin/mksfs lhd1raw: mydrive; mount sfs lhd1:; cd lhd1:; doom 2; p /testbin/filetest'; sys161 kernel 'mount sfs lhd1:; cd lhd1:; p /sbin/sfsck lhd1raw:='