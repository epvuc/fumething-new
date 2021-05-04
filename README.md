# fumething-new
rewrite of `esp-fume-detector` for esp-idf 4.2

 
 * Instead of using external "esp32-wifi-manager", now using esp-idf 4.2's builtin Wi-Fi Provisioning Manager, which you can setup using the android or iOS apps. 
 
 * implement a minimal API using idf's httpd:
   - `/fume` - dump json blob of version, dest_ip, dest_port, interval, gl_count, gl_fumes, gl_temp, gl_pressure, gl_humidity
   - `/fume?dest_ip=a.b.c.d` - set logging target IP
   - `/fume?dest_port=N` - set logging target port number
   - `/fume?interval=N` - set how often to send a measurement, in seconds
   - `/fume?reset` - reset 
  
 
