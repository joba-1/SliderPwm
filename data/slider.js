// slider with server callbacks
// slider id 'slider'
// slider amount element id 'sliderAmount'
// slider form id 'form'
// slider url receiving the slider position '/change'

var slider = document.getElementById('slider');
var sliderDiv = document.getElementById('sliderAmount');
var sliderBusy = 0;

slider.oninput = function() {
 sliderDiv.innerHTML = this.value;
 if (sliderBusy == 0) {
  sliderBusy = 1;
  $.post({
   url: '/change',
   data: $('form').serialize(),
   success: function(response) { sliderBusy = 0; },
   error: function(error) { sliderBusy = 0; console.log(error); }
  });
 }
}

slider.onchange = function() {
 $.post({
  url: '/change',
  data: $('form').serialize(),
  success: function(response) { sliderBusy = 0; },
  error: function(error) { sliderBusy = 0; console.log(error); }
 });
}
