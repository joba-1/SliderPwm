from flask import Flask, request, url_for, render_template_string, redirect

slider_value = '50'  # start value

app = Flask(__name__, static_folder="data")

@app.route('/change', methods=['POST'])
def change():
  print('request.form:', request.form)
  if request.form.get('button') == 'button-1':
    print("button 1 pressed")
  elif request.form.get('button') == 'button-2':
    print("button 2 pressed")
  elif request.form.get('slider'):
    global slider_value
    slider_value = request.form.get('slider')
    print(f'slider changed to {slider_value}')
  return redirect('/')

@app.route('/', methods=['GET'])
def root():
  bootstrap_css = url_for('static', filename='bootstrap.min.css.gz')
  bootstrap_js = url_for('static', filename='bootstrap.bundle.min.js.gz')
  jquery_js = url_for('static', filename='jquery.min.js.gz')
  global slider_value
  # favicon converted and optimized from 16x16-png by https://www.base64-image.de/
  return render_template_string(f'''<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQAgMAAABinRfyAAAADFBMVEUqYbutnpTMuq/70SQgIef5AAAAVUlEQVQIHWOAAPkvDAyM3+Y7MLA7NV5g4GVqKGCQYWowYTBhapBhMGB04GE4/0X+M8Pxi+6XGS67XzzO8FH+iz/Dl/q/8gx/2S/UM/y/wP6f4T8QAAB3Bx3jhPJqfQAAAABJRU5ErkJggg==" rel="icon" type="image/x-icon" />
    <link href="{bootstrap_css}" rel="stylesheet">
    <title>Slider Callback</title>
  </head>

  <body>
    <form action="/change" method ="post" enctype="multipart/form-data" id="form">
      <div class="container">
        <div class="row">
          <div class="col-12">
            <h1>Slider Callback</h1>
          </div>
        </div>

        <div class="row">
          <div class="col-9">
            <input style="width:100%" id="slider" type="range" min="1" max="100" step="1" value="{slider_value}" name="slider">
          </div>
          <div class="col-3" >
            <div id="sliderAmount">{slider_value}</div>
          </div>
        </div>

        <div class="row">
          <div class="col-9">
            <button class="btn btn-primary" button type="submit" name="button" value="button-1">ONE</button>
          </div>
          <div class="col-3">
            <button class="btn btn-primary" button type="submit" name="button" value="button-2">TWO</button>
          </div>
        </div>
      </div>
    </form>

    <script src="{jquery_js}"></script>
    <script src="{bootstrap_js}"></script>
  </body>

  <script>
    var slider = document.getElementById('slider');
    var sliderDiv = document.getElementById("sliderAmount");

    slider.oninput = function() {{
      sliderDiv.innerHTML = this.value;
    }}

    slider.onchange = function() {{
      $.post({{
        url: '/change',
        data: $('form').serialize(),
        success: function(response) {{ console.log(response); }},
        error: function(error) {{ alert(response); console.log(error); }}
      }});
    }}
  </script>
</html>''')

if __name__ == '__main__':
    app.run(host="0.0.0.0", debug=True, use_reloader=True)
