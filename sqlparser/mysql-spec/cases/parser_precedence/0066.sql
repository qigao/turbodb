select count(*) from t1_30237_bool
  where (A OR (B AND C)) != (A OR B AND C);
