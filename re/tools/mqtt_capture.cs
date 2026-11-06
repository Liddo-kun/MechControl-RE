using System;
using System.Text;
using System.Threading;
using System.IO;
using System.Collections.Generic;
using uPLibrary.Networking.M2Mqtt;
using uPLibrary.Networking.M2Mqtt.Messages;

class Program
{
    static readonly object lockObj = new object();
    static StreamWriter logWriter;
    static int messageCount = 0;
    static Dictionary<string, int> topicCounts = new Dictionary<string, int>();

    static void Main(string[] args)
    {
        string logFile = Path.Combine(Path.GetDirectoryName(
            System.Reflection.Assembly.GetExecutingAssembly().Location),
            "mqtt_capture_output.txt");

        logWriter = new StreamWriter(logFile, false, Encoding.UTF8);
        logWriter.AutoFlush = true;

        Log("MECHREVO MQTT Capture - " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));

        MqttClient client = null;
        try
        {
            client = new MqttClient("127.0.0.1", 13688, false, null, null, MqttSslProtocols.None);

            client.MqttMsgPublishReceived += delegate(object sender, MqttMsgPublishEventArgs e) {
                string topic = e.Topic;
                string payload = Encoding.UTF8.GetString(e.Message);
                lock (lockObj)
                {
                    messageCount++;
                    if (topicCounts.ContainsKey(topic))
                        topicCounts[topic]++;
                    else
                        topicCounts[topic] = 1;
                }
                Log("[" + topic + "] " + payload);
            };

            byte code = client.Connect("OcTool", "OcToolUser", "OcToolPwd123");
            Log("Connected: code=" + code + " isConnected=" + client.IsConnected);

            client.Subscribe(new string[] { "#" }, new byte[] { 0 });
            Thread.Sleep(2000);

            string q = "{\"Action\":\"GETSTATUS\"}";
            byte[] qb = Encoding.UTF8.GetBytes(q);

            string[] topics = new string[] {
                "Fan/Control", "Setting/Control", "BatteryProtection/Control",
                "EC/Control", "Keyboard/Ctrl", "System/Control",
                "HidLightbar/Ctrl", "HidLightbar_Hinge/Ctrl", "HidLightbar_Logo/Ctrl"
            };

            foreach (string t in topics)
            {
                Log(">> Publishing to " + t);
                client.Publish(t, qb);
                Thread.Sleep(500);
            }

            Thread.Sleep(5000);

            Log("\n=== SUMMARY: " + messageCount + " messages ===");
            lock (lockObj)
            {
                foreach (KeyValuePair<string, int> kvp in topicCounts)
                    Log("  " + kvp.Key + ": " + kvp.Value);
            }
        }
        catch (Exception ex)
        {
            Log("ERROR: " + ex.Message);
        }
        finally
        {
            if (client != null && client.IsConnected)
                client.Disconnect();
            logWriter.Flush();
            logWriter.Close();
        }
    }

    static void Log(string msg)
    {
        Console.WriteLine(msg);
        lock (lockObj) { logWriter.WriteLine(msg); }
    }
}
