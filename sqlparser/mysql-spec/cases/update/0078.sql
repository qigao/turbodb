delete from m1 using t1 m1,t1 m2 where m1.groupid=m2.groupid and (m1.cnt < m2.cnt or m1.cnt=m2.cnt and m1.F3>m2.F3);
