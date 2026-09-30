create table t1 (a int not null auto_increment, primary key (a), t timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, c char(10) default "hello", i int);
