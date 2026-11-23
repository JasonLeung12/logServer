一、数据库monitor_data.db中存储了使用的数据，里面共有四张表
1、clients:保存的是客户端的ip和计算机名
2、row_logs：保存的是各个客户端的心跳记录和关键操作记录（0：心跳   1：关键操作）
3、license_daily_usage:记录了每个客户端的有效时长
4、concurrency_stats：并发统计表：记录每天是否发生过并发（即是否有过重叠的在线时段），即所有客户端是否同时在线

二、whitelist.txt中保存的是可连接的客户端的数量
三、客户端build/bin/config.ini下保存的是服务器的ip和端口，ip可根据实际情况做更改

