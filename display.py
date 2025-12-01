from flask import Flask, render_template_string, jsonify
from datetime import datetime

app = Flask(__name__)

# 食事状態を保存するグローバル変数
meal_status = {
    "status": "食事未検出",
    "start_time": None,
    "end_time": None,
    "duration": "0分"
}

# HTMLテンプレート（日本語対応）
HTML_TEMPLATE = """
<!DOCTYPE html>
<html>
<head>
    <title>食事状態監視システム</title>
    <meta charset="utf-8">
    <style>
        body { 
            font-family: "メイリオ", Meiryo, sans-serif;
            text-align: center; 
            margin-top: 30px; 
            background-color: #f5f5f5;
        }
        .dashboard {
            background: white;
            padding: 30px;
            border-radius: 15px;
            box-shadow: 0 0 20px rgba(0,0,0,0.1);
            display: inline-block;
        }
        .status-indicator {
            font-size: 28px;
            padding: 20px 40px;
            border-radius: 10px;
            margin: 20px 0;
        }
        .eating { background: #4CAF50; color: white; }
        .not-eating { background: #F44336; color: white; }
        .timestamps {
            color: #666;
            margin: 20px 0;
            font-size: 16px;
        }
        button {
            background: #2196F3;
            color: white;
            border: none;
            padding: 12px 25px;
            border-radius: 5px;
            cursor: pointer;
            font-size: 16px;
            margin: 10px;
            transition: all 0.3s;
        }
        button:hover { opacity: 0.9; transform: scale(1.05); }
        #countdown {
            color: #FF9800;
            font-weight: bold;
            margin: 15px 0;
        }
    </style>
</head>
<body>
    <div class="dashboard">
        <h1>食事状態監視ダッシュボード</h1>
        
        <div class="status-indicator" id="statusIndicator">
            <span id="statusText">状態取得中...</span>
        </div>

        <div class="timestamps">
            <div>▶️ 開始時刻：<span id="startTime">-</span></div>
            <div>⏹️ 終了時刻：<span id="endTime">-</span></div>
            <div>⏱️ 食事時間：<span id="duration">-</span></div>
        </div>

        <div id="countdown"></div>
        
        <button onclick="resetStatus()">手動リセット</button>
    </div>

    <script>
        // システム設定
        const TIMEOUT_MINUTES = 0.5
        let timeoutTimer = null
        let hasAlerted = false

        function startCountdown() {
            const countdownElement = document.getElementById('countdown')
            let remainingSeconds = TIMEOUT_MINUTES * 60

            const updateDisplay = () => {
                const minutes = Math.floor(remainingSeconds / 60)
                const seconds = remainingSeconds % 60
                countdownElement.textContent = `⏳ 残り待機時間：${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}`
            }

            if(timeoutTimer) clearInterval(timeoutTimer)

            timeoutTimer = setInterval(() => {
                remainingSeconds -= 1
                updateDisplay()
                
                if(remainingSeconds <= 0) {
                    clearInterval(timeoutTimer)
                    checkMealStatus()
                }
            }, 1000)
        }

        async function checkMealStatus() {
            try {
                const response = await fetch('/status')
                const data = await response.json()
                
                if(data.status === '食事未検出' && !hasAlerted) {
                    alert('⚠️ 警告：食事開始を検出できませんでした！')
                    hasAlerted = true
                }
            } catch (error) {
                console.error('状態取得エラー:', error)
            }
        }

        function updateStatus(data) {
            const indicator = document.getElementById('statusIndicator')
            indicator.className = `status-indicator ${data.status === '食事中' ? 'eating' : 'not-eating'}`
            
            document.getElementById('statusText').textContent = data.status
            document.getElementById('startTime').textContent = data.start_time || '-'
            document.getElementById('endTime').textContent = data.end_time || '-'
            document.getElementById('duration').textContent = data.duration

            if(data.status === '食事中') {
                hasAlerted = false
                startCountdown()
            }
        }

        async function fetchStatus() {
            try {
                const response = await fetch('/status')
                const data = await response.json()
                updateStatus(data)
            } catch (error) {
                console.error('状態更新エラー:', error)
            }
        }

        async function resetStatus() {
            if(confirm('本当に全状態をリセットしますか？')) {
                try {
                    await fetch('/reset')
                    hasAlerted = false
                    startCountdown()
                    fetchStatus()
                } catch (error) {
                    alert('リセット失敗')
                }
            }
        }

        // 初期化
        startCountdown()
        setInterval(fetchStatus, 1000)
        fetchStatus()
    </script>
</body>
</html>
"""

@app.route('/')
def index():
    return render_template_string(HTML_TEMPLATE, **meal_status)

@app.route('/mealStart')
def start_meal():
    meal_status.update({
        "status": "食事中",
        "start_time": datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
        "end_time": None,
        "duration": "0分"
    })
    return "食事開始時刻を記録しました", 200

@app.route('/mealEnd')
def finish_meal():
    start_time = meal_status["start_time"]
    end_time = datetime.now()
    
    if start_time:
        start = datetime.strptime(start_time, '%Y-%m-%d %H:%M:%S')
        duration = end_time - start
        mins, secs = divmod(duration.seconds, 60)
        meal_status["duration"] = f"{mins}分{secs}秒"

    meal_status.update({
        "status": "食事未検出",
        "end_time": end_time.strftime('%Y-%m-%d %H:%M:%S')
    })
    return "食事終了時刻を記録しました", 200

@app.route('/reset')
def reset_status():
    meal_status.update({
        "status": "食事未検出",
        "start_time": None,
        "end_time": None,
        "duration": "0分"
    })
    return "システム状態をリセットしました", 200

@app.route('/status')
def get_status():
    return jsonify(meal_status)

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=50000, debug=True)