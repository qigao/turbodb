with qn as (select * from t1) select (select max(a) from qn);  
