insert into t2 (t2) select distinct substring(email, locate('@', email)+1) from t1;
