update t1 set b=(select distinct 1 from (select * from t2) a);
