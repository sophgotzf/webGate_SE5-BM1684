一、linux终端(192.168.10.200)
scp hello-*-aarch64-full-*.tar.gz linaro@192.168.150.1:/data/  #将压缩包hello-v1.3.6-aarch64-full-20261006.tar.gz拷贝到/data/目录下
tar -xvf dist/hello-v1.3.6-aarch64-full-20261006.tar.gz
sudo chown -R linaro:linaro /data/hello
cd hello

sh DEPLOY/check_env.sh                           # 只读体检，不改任何东西
sh DEPLOY/llm_web_ctl.sh start                   # 起 llm_shell daemon + llm_web
sh DEPLOY/verify.sh                              # 端到端验收（约 15 秒，26 项）

开机自启:
cp DEPLOY/systemd/llm-shell.service /etc/systemd/system/
sudo systemctl enable llm-shell
sudo systemctl restart llm-shell
sudo systemctl status llm-shell

cp DEPLOY/systemd/llm-web.service /etc/systemd/system/
sudo systemctl enable llm-web
sudo systemctl restart llm-web
sudo systemctl status llm-web

sudo systemctl daemon-reload


cd /data/hello
sh DEPLOY/llm_web_ctl.sh start      
sh DEPLOY/llm_web_ctl.sh status    
sh DEPLOY/llm_web_ctl.sh health    
sh DEPLOY/llm_web_ctl.sh logs      
sh DEPLOY/llm_web_ctl.sh restart
sh DEPLOY/llm_web_ctl.sh stop      

***确保linux终端网络与外网互通，以便访问大模型***


服务状态查询:
sudo systemctl status llm-shell 
sudo systemctl status llm-web
或
systemctl status llm-shell llm-web

二、windows端测试
cmd:
  ssh -L 8093:127.0.0.1:8093 linaro@192.168.10.200
web：
  127.0.0.1:8093
