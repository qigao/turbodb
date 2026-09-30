select A, B, C, (A OR B) OR C, A OR (B OR C), A OR B OR C
 from t1_30237_bool order by A, B, C;
