// function to add server callbacks on value input or change to sliders

function sliderCallback(sliderId, valueId, callbackUrl) {

    var slider = document.getElementById(sliderId);
    var value = document.getElementById(valueId);

    slider.oninput = function() {
        value.innerHTML = this.value;
        if (!this.hasAttribute('data-busy')) {
            this.setAttribute('data-busy', '');
            $.post({
                url: callbackUrl,
                data: sliderId + "=" + this.value,
                success: function() { slider.removeAttribute('data-busy'); },
                error: function(error) { slider.removeAttribute('data-busy'); console.log(error); }
            });
        }
    }

    slider.onchange = function() {
        $.post({
            url: callbackUrl,
            data: sliderId + "=" + this.value,
            success: function() { slider.removeAttribute('data-busy'); },
            error: function(error) { slider.removeAttribute('data-busy'); console.log(error); }
        });
    }
}
