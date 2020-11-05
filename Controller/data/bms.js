$(function () {
    $("#email").click(function() {$.ajax({
        type: "POST",
        url: "/email"
    });});
    $("#menu > a").on('click',function (event) {
        $('#menu > a').removeClass("active");
        $(this).addClass("active");
        showContent($(this).attr("href"));
        event.preventDefault();
        }
    );
    $("#settingsMenu").on('click',getSettings);
    $("#battMenu").on('click',getSettings);
    $("#netMenu").on('click',getSettings);
    showContent("cell");
    initChart();
    Setup();
    queryBMS();
});

var RELAY_TOTAL = 6;
var myChart,config;

function showContent(c) {
    $("#content > div").hide();
    $("#"+c).show();
}

function initChart() {
    config = {
        type: 'line',
        data: {
            labels: [ ],
            datasets: []
        },
        options: {
            responsive: true,
            title: {
                display: true,
                text: 'Cell Voltages'
            },
            tooltips: {
                mode: 'index',
                intersect: false,
            },
            hover: {
                mode: 'nearest',
                intersect: true
            },
            scales: {
                xAxes: [{
                    type: 'time',
                    time: {
                        parser: 'HH:mm:ss',
                        unit: 'second'
                    },
                    scaleLabel: {
                        display: true,
                        labelString: 'Date'
                    }
                }],
                yAxes: [{
                    display: true,
                    scaleLabel: {
                        display: true,
                        labelString: 'Volts'
                    },
                    ticks: { max: 3.7, min: 2.5 }
                }]
            }
        }
    };
    myChart = new Chart(document.getElementById('cellCan'), config);
}

var colors = [ '#e6194B', '#3cb44b', '#ffe119', '#4363d8', '#f58231', '#42d4f4', '#f032e6', '#fabed4', '#469990', '#dcbeff', '#9A6324', '#fffac8', '#800000', '#aaffc3', '#000075', '#a9a9a9', '#ffffff', '#000000' ];

function initCells(nBanks,nCells) {
    config.data.labels = [];
    config.data.datasets = [];
    for (var i=0;i<nCells;i++) {
        config.data.datasets[i] = {
            label: 'Cell '+i,
            backgroundColor: colors[i],
            borderColor: colors[i],
            data: [ ],
            fill: false
        }
    }
    myChart.update();
}

function getSettings() {
    $.getJSON("settings",
        function (data) {
            $("input[name='email']").val(data.email);
            $("input[name='senderEmail']").val(data.senderEmail);
            $("input[name='senderServer']").val(data.senderServer);
            $("input[name='senderPort']").val(data.senderPort);
            $("input[name='senderSubject']").val(data.senderSubject);
            $("#Avg").val(data.Avg);
            $("#ConvTime").val(data.ConvTime);
            $("#BattAH").val(data.BattAH);
            $("#socLastAdj").html(data.socLastAdj);
            $("#socAvgAdj").html(data.socAvgAdj);
            $("#BatAHMeasured").html(data.BatAHMeasured);
            $("#MaxAmps").val(data.MaxAmps);
            $("#PVMaxAmps").val(data.PVMaxAmps);
            $("#ShuntUOhms").val(data.ShuntUOhms);
            $("#PVShuntUOhms").val(data.PVShuntUOhms);
            $("#nBanks").val(data.nBanks);
            $("#nCells").val(data.nCells);

            $("#CurSOC").val('');
            $("#PollFreq").val(data.PollFreq);
            $("input[name='ssid'").val(data.ssid);
            
            $.each(data.limitSettings, function (index, value) {
                $("#" + index).val(value);
            });
            $("#useex").prop("checked", data.useex);

            $.each(data.relaySettings, function (index, value) {
                $("#relayName" + index).val(value.name);
                $("#relayType" + index).val(value.type);
                $("#relayDoSOC" + index).prop("checked", value.doSOC);
                $("#relayTrip" + index).val(value.trip);
                $("#relayRec" + index).val(value.rec);
            });
    
        }).fail(function () { }
        );
    return true;
}

var first = true;
function queryBMS() {
    $.getJSON("status" + (first ? "1" : ""), function (data) {
        first = false;
        for (var i=0;i<RELAY_TOTAL;i++) {
            var rn = data["relayName"+i];
            if (rn && rn.length) {
                $("#relayStatus"+i).show();
                $("#relayStatus"+i+" .t").html(rn);
                $("#relayStatus"+i+" .v").html(data["relayStatus"+i]);
                if (data["relayOff"+i] == "off")
                    $("#relayStatus"+i+" .v").addClass("manoff");
                else $("#relayStatus"+i+" .v").removeClass("manoff");
            } else $("#relayStatus"+i).hide();
        }
        $("#debuginfo").html(data.debuginfo);

        var d = new Date(1000 * data.now);
        $("#timenow").html(d.toLocaleString());

        $("#cellvolt").hide();
        if (data.maxCellVState || data.minCellVState)
            $("#cellvolt").show();
        val = "";
        if (data.maxCellVState)
            val = "over";
        if (data.minCellVState)
            val = val + " under";
        $("#cellvolt .v").html(val);

        var val = data.packvolts;
        if (data.maxPackVState || data.minPackVState)
            $("#packvolts .v").addClass("highlighted");
        else $("#packvolts .v").removeClass("highlighted");
        if (data.maxPackVState)
            val = "over " + val;
        if (data.minPackVState)
            val = "under " + val;
        $("#packvolts .v").html(val);

        $("#packcurrent .v").html(data.packcurrent);
        $("#pvcurrent .v").html(data.pvcurrent);
        $("#loadcurrent .v").html(data.loadcurrent);
        $("#soc .v").html(data.soc);
        if (data.socvalid)
            $("#soc .v").removeClass("manoff");
        else $("#soc .v").addClass("manoff");

        $("#celltemp").hide();
        if (data.maxCellCState || data.minCellCState)
            $("#celltemp").show();
        val = "";
        if (data.maxCellCState)
            val = "over";
        if (data.minCellCState)
            val = val + " under";
        $("#celltemp .v").html(val);

        val = data.temp1;
        if (data.maxPackCState || data.minPackCState)
            $("#temp1 .v").addClass("highlighted")
        else $("#temp1 .v").removeClass("highlighted")
        if (data.maxPackCState)
            val = "over " + val;
        if (data.minPackCState)
            val = "under " + val;
        $("#temp1 .v").html(val);
        val = data.temp2;
        if (val < -100)
            $("#temp2").hide();
        else {
            $("#temp2").show();
            $("#temp2 .v").html(data.temp2);
        }
        $("#uptime .v").html(data.uptime);

        if (data.nocells == true) {
            $("#nocells").show();
        } else {
            $("#nocells").fadeOut();
        }
        if (data.pkts) $("#pkts .v").html(data.pkts);

        $("#nocontroller").hide();

        if (data.nCells !== undefined) {
            initCells(data.nBanks,data.nCells);
        }
        if (data.bank)
        $.each(data.bank, function (index, value) { // does not handle multiple banks, multiple charts are needed
            config.data.labels.push(new Date());
            if (config.data.labels.length > 20)
                config.data.labels.shift();
            $.each(value, function (index, value) {
                var data = config.data.datasets[index].data;
                data.push(value.v / 1000.0);
                if (data.length > 20)
                    data.shift();
            }); 
            myChart.update();
        });
    }).fail(function () {
        $("#nocontroller").show();
    });
  setTimeout(queryBMS, 2000);
}

function convertTemp(t) {
    var t = $(t + " .v").html();
    if (data.val) t = (t-32)*5/9;
    else t = t*9/5 + 32;
    $(t + " .v").html(Math.round(t));
}

function toggleTemp() {
    $.ajax({
        type: "GET",
        url: "/toggleTemp",
        success: function (data) {
            convertTemp("#temp1");
            convertTemp("#temp2");
            $("#savesuccess").show().delay(2000).fadeOut(500);
        },
        error: function (data) {
            $("#saveerror").show().delay(2000).fadeOut(500);
        }
    });
    return false;
}

function Setup() {

    $("#loading").show();

    $("#temp1 a").click(toggleTemp);
    $("#temp2 a").click(toggleTemp);
    var LIMITNames = [['Volts (mV)','Temp (C)'],['Cell','Pack'],['Max','Min'],['Trip:','Rec:']];
    for (var l0=0;l0<LIMITNames[0].length;l0++) {
        for (var l1=0;l1<LIMITNames[1].length;l1++) {
            for (var l2=0;l2<LIMITNames[2].length;l2++) {
                var d = $("<div class='limits'></div>");
                for (var l3=0;l3<LIMITNames[3].length;l3++) {
                    var id=l0+""+l1+""+l2+""+l3;
                    var temp = $("#limit").clone();
                    var lab = temp.find("label");
                    if (l3) {
                        lab.html(LIMITNames[3][l3]);
                        lab.addClass('rec');
                    } else {
                        lab.addClass('field');
                        lab.html(LIMITNames[2][l2] + ' ' + LIMITNames[1][l1] + ' ' + LIMITNames[0][l0]);
                    }
                    var inp=temp.find("input");
                    inp.attr("for",id);
                    inp.attr("id",id);
                    inp.attr("name",id);
                    d.append(temp.children());
                }
                d.insertBefore("#limit");
            }
        }
    }
    $("#limit").remove();
    
    //Duplicate the template and then kill it.
    for (var rel=0;rel<RELAY_TOTAL;rel++) {
        var temp = $("#relay").clone();
        temp.attr({id: "relay"+rel});
        temp.find("[for='relayName']").text("J"+rel+":");
        $.each(['Name','DoSOC','Type','Trip','Rec'],function (index,value) {
            temp.find('#relay'+value).attr({id: "relay"+value+rel, name: "relay"+value+rel});
        });
        temp.find("[for]").each(function(index) {
            var val=$(this).attr("for");
            $(this).attr({for: val+rel});
        });
        temp.insertBefore("#relay");
        temp = $("#relayStatus").clone();
        temp.attr({id: "relayStatus"+rel});
        var theA = temp.find("a");
        theA.attr("relay",rel);
        theA.click(function () {
            var r = $(this).attr("relay");
            $.ajax({
                type: "POST",
                url: "/saveOff",
                data: { relay: r},
                dataType:'json',
                success: function (data) {
                    var r = data.relay;
                    if (data.val == "off")
                        $("#relayStatus"+r+" .v").addClass("manoff");
                    else $("#relayStatus"+r+" .v").removeClass("manoff");
                    $("#savesuccess").show().delay(2000).fadeOut(500);
                },
                error: function (data) {
                    $("#saveerror").show().delay(2000).fadeOut(500);
                }
            });
            return false;
         });
        temp.insertBefore("#relayStatus");
    }
    $("#relay").remove();
    $("#relayStatus").remove();

    $('#kickWifi').click(function () {
        $.get("/kickwifi", function () { });
    });

    $("form").unbind('submit').submit(function (e) {
        e.preventDefault();
    
        $.ajax({
            type: $(this).attr('method'),
            url: $(this).attr('action'),
            data: $(this).serialize(),
            success: function (data) {
                $("#savesuccess").show().delay(2000).fadeOut(500);
            },
            error: function (data) {
                $("#saveerror").show().delay(2000).fadeOut(500);
            },
        });
    });
        
}