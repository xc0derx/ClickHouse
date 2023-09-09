set prefer_localhost_replica=0;

drop table if exists data;
drop table if exists dist;

-- { echoOn }

create table data (key Int) engine=Memory();
create table dist (key Int) engine=Distributed(default, currentDatabase(), data);
system stop distributed sends dist;

-- check that FLUSH DISTRIBUTED does flushing anyway
insert into dist values (1);
select * from data;
system flush distributed dist;
select * from data;
truncate table data;

-- check that flush_on_detach=1 by default
insert into dist values (1);
detach table dist;
select * from data;
attach table dist;
truncate table data;

-- check flush_on_detach=0
drop table dist;
create table dist (key Int) engine=Distributed(default, currentDatabase(), data) settings flush_on_detach=0;
system stop distributed sends dist;
insert into dist values (1);
detach table dist;
select * from data;
attach table dist;
