--
-- XC_FOR_UPDATE
--

set enable_fast_query_shipping=true;

-- create some tables
create table t1(val int, val2 int);
create table t2(val int, val2 int);
create table t3(val int, val2 int);

create table p1(a int, b int);
create table c1(d int, e int) inherits (p1);

-- insert some rows in them
insert into t1 values(1,11),(2,11);
insert into t2 values(3,11),(4,11);
insert into t3 values(5,11),(6,11);

insert into p1 values(55,66),(77,88);
insert into c1 values(111,222,333,444),(123,345,567,789);

select * from t1 order by val;
select * from t2 order by val;
select * from t3 order by val;
select * from p1 order by a;
select * from c1 order by a;

-- create a view too
create view v1 as select * from t1 for update;

-- test a few queries with row marks
select * from t1 order by 1 for update of t1 nowait;
select * from t1, t2, t3 order by 1 for update;

-- drop objects created
drop table c1;
drop table p1;
drop view v1;
drop table t1;
drop table t2;
drop table t3;

---------------------------------------------------
-- updatable_views

-- WITH CHECK OPTION with subquery

CREATE TABLE base_tbl (a int) DISTRIBUTE BY REPLICATION;
CREATE TABLE ref_tbl (a int PRIMARY KEY) DISTRIBUTE BY REPLICATION;
INSERT INTO ref_tbl SELECT * FROM generate_series(1,10);

CREATE VIEW rw_view1 AS
  SELECT * FROM base_tbl b
  WHERE EXISTS(SELECT 1 FROM ref_tbl r WHERE r.a = b.a)
  WITH CHECK OPTION;

INSERT INTO rw_view1 VALUES (5); -- ok

drop view rw_view1;
drop table ref_tbl;
drop table base_tbl;
--------------------------------------------------

-- temp test
-- Test ON COMMIT DELETE ROWS

CREATE TEMP TABLE temptest(col int) ON COMMIT DELETE ROWS;

BEGIN;
INSERT INTO temptest VALUES (1);
INSERT INTO temptest VALUES (2);

SELECT * FROM temptest  ORDER BY 1;
COMMIT;

SELECT * FROM temptest;

DROP TABLE temptest;
---------------------------------------------------

-- from xc_remote test

-- Test for remote DML on different tables
CREATE TABLE rel_rep (a int, b int) DISTRIBUTE BY REPLICATION;
CREATE TABLE rel_hash (a int, b int) DISTRIBUTE BY HASH (a);
CREATE TABLE rel_rr (a int, b int) DISTRIBUTE BY ROUNDROBIN;
CREATE SEQUENCE seqtest START 10;
CREATE SEQUENCE seqtest2 START 100;

-- INSERT cases
INSERT INTO rel_rep VALUES (1,1);
INSERT INTO rel_hash VALUES (1,1);
INSERT INTO rel_rr VALUES (1,1);

-- Multiple entries with non-shippable expressions
INSERT INTO rel_rep VALUES (nextval('seqtest'), nextval('seqtest')), (1, nextval('seqtest'));
INSERT INTO rel_rep VALUES (nextval('seqtest'), 1), (nextval('seqtest'), nextval('seqtest2'));
INSERT INTO rel_hash VALUES (nextval('seqtest'), nextval('seqtest')), (1, nextval('seqtest'));
INSERT INTO rel_hash VALUES (nextval('seqtest'), 1), (nextval('seqtest'), nextval('seqtest2'));
INSERT INTO rel_rr VALUES (nextval('seqtest'), nextval('seqtest')), (1, nextval('seqtest'));
INSERT INTO rel_rr VALUES (nextval('seqtest'), 1), (nextval('seqtest'), nextval('seqtest2'));

-- Global check
SELECT a, b FROM rel_rep ORDER BY 1,2;
SELECT a, b FROM rel_hash ORDER BY 1,2;
SELECT a, b FROM rel_rr ORDER BY 1,2;

-- Some SELECT queries with some quals
-- Coordinator quals first
SELECT a, b FROM rel_rep WHERE a <= currval('seqtest') - 15 ORDER BY 1,2;

DROP TABLE rel_rep;
DROP TABLE rel_hash;
DROP TABLE rel_rr ;
DROP SEQUENCE seqtest;
DROP SEQUENCE seqtest2;

--------------------------------

-- from plpgsql test


create temp table foo (f1 int);

create function subxact_rollback_semantics() returns int as $$
declare x int;
begin
  x := 1;
  insert into foo values(x);
  begin
    x := x + 1;
    insert into foo values(x);
    raise exception 'inner';
  exception
    when others then
      x := x * 10;
  end;
  insert into foo values(x);
  return x;
end$$ language plpgsql;

select subxact_rollback_semantics();

drop function subxact_rollback_semantics();

------------------------------------------

-- from xc_misc

-- Test an SQL function with multiple statements in it including a utility statement.

create table my_tab1 (a int);

insert into my_tab1 values(1);

create function f1 () returns setof my_tab1 as $$ create table my_tab2 (a int); select * from my_tab1; $$ language sql;

SET check_function_bodies = false;

create function f1 () returns setof my_tab1 as $$ create table my_tab2 (a int); select * from my_tab1; $$ language sql;

select f1();

SET check_function_bodies = true;

drop function f1();
drop table my_tab1;

--------------------------------------------------
-- 
-- versions
--

-- Bugs related to pushing down volatile functions to datanodes - copied from
-- misc.sql
--
-- postquel functions
--
--
-- mike does post_hacking,
-- joe and sally play basketball, and
-- everyone else does nothing.
--
SELECT p.name, name(p.hobbies) FROM ONLY person p ORDER BY 1,2;

--
-- as above, but jeff also does post_hacking.
--
SELECT p.name, name(p.hobbies) FROM person* p ORDER BY 1,2;

--
-- the next two queries demonstrate how functions generate bogus duplicates.
-- this is a "feature" ..
--
SELECT DISTINCT hobbies_r.name, name(hobbies_r.equipment) FROM hobbies_r
  ORDER BY 1,2;

SELECT hobbies_r.name, (hobbies_r.equipment).name FROM hobbies_r ORDER BY 1,2;

--
-- mike needs advil and peet's coffee,
-- joe and sally need hightops, and
-- everyone else is fine.
--
SELECT p.name, name(p.hobbies), name(equipment(p.hobbies)) FROM ONLY person p ORDER BY 1,2,3;

--
-- as above, but jeff needs advil and peet's coffee as well.
--
SELECT p.name, name(p.hobbies), name(equipment(p.hobbies)) FROM person* p ORDER BY 1,2,3;

--
-- just like the last two, but make sure that the target list fixup and
-- unflattening is being done correctly.
--
SELECT name(equipment(p.hobbies)), p.name, name(p.hobbies) FROM ONLY person p ORDER BY 1,2,3;

SELECT (p.hobbies).equipment.name, p.name, name(p.hobbies) FROM person* p ORDER BY 1,2,3;

SELECT (p.hobbies).equipment.name, name(p.hobbies), p.name FROM ONLY person p ORDER BY 1,2,3;

SELECT name(equipment(p.hobbies)), name(p.hobbies), p.name FROM person* p ORDER BY 1,2,3;

SELECT user_relns() AS user_relns
   ORDER BY user_relns;

SELECT name(equipment(hobby_construct(text 'skywalking', text 'mer')));

SELECT name(equipment(hobby_construct_named(text 'skywalking', text 'mer')));

SELECT name(equipment_named(hobby_construct_named(text 'skywalking', text 'mer')));

SELECT name(equipment_named_ambiguous_1a(hobby_construct_named(text 'skywalking', text 'mer')));

SELECT name(equipment_named_ambiguous_1b(hobby_construct_named(text 'skywalking', text 'mer')));

SELECT name(equipment_named_ambiguous_1c(hobby_construct_named(text 'skywalking', text 'mer')));

SELECT name(equipment_named_ambiguous_2a(text 'skywalking'));

SELECT name(equipment_named_ambiguous_2b(text 'skywalking')) ORDER BY 1;

SELECT hobbies_by_name('basketball');

SELECT name, overpaid(emp.*) FROM emp ORDER BY 1,2;

--
-- Try a few cases with SQL-spec row constructor expressions
--
SELECT * FROM equipment(ROW('skywalking', 'mer'));

SELECT name(equipment(ROW('skywalking', 'mer')));

SELECT *, name(equipment(h.*)) FROM hobbies_r h ORDER BY 1,2,3;

SELECT *, (equipment(CAST((h.*) AS hobbies_r))).name FROM hobbies_r h ORDER BY 1,2,3;
