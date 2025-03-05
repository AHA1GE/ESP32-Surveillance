import os
from flask import Flask, send_from_directory, render_template_string

VIDEOS_DIR = '/videos'

app = Flask(__name__)

@app.route('/')
def index():
    """
    Index route that lists all available MP4 videos in the VIDEOS_DIR.
    """
    try:
        files = os.listdir(VIDEOS_DIR)
        # Filter only mp4 files
        videos = [f for f in files if f.lower().endswith('.mp4')]
    except Exception as e:
        videos = []
    
    # Minimal HTML template for listing videos
    html = """
    <!DOCTYPE html>
    <html>
    <head>
        <title>Available Videos</title>
    </head>
    <body>
        <h1>Available Videos</h1>
        <ul>
        {% for video in videos %}
            <li><a href="/videos/{{ video }}">{{ video }}</a></li>
        {% endfor %}
        </ul>
    </body>
    </html>
    """
    return render_template_string(html, videos=videos)

@app.route('/videos/<path:filename>')
def serve_video(filename):
    """
    Serve a video file from the VIDEOS_DIR.
    """
    return send_from_directory(VIDEOS_DIR, filename)

if __name__ == '__main__':
    # Run on all network interfaces at port 80
    app.run(host='0.0.0.0', port=80)
